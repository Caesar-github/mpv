/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <assert.h>

#include <libavutil/mem.h>
#include <libavutil/common.h>
#include <libavutil/bswap.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>

#include "mpv_talloc.h"

#include "img_format.h"
#include "mp_image.h"
#include "sws_utils.h"
#include "fmt-conversion.h"
#include "gpu_memcpy.h"

#include "video/filter/vf.h"

static bool mp_image_alloc_planes(struct mp_image *mpi)
{
    assert(!mpi->planes[0]);
    assert(!mpi->bufs[0]);

    if (!mp_image_params_valid(&mpi->params) || mpi->fmt.flags & MP_IMGFLAG_HWACCEL)
        return false;

    // Note: for non-mod-2 4:2:0 YUV frames, we have to allocate an additional
    //       top/right border. This is needed for correct handling of such
    //       images in filter and VO code (e.g. vo_vdpau or vo_opengl).

    size_t plane_size[MP_MAX_PLANES];
    for (int n = 0; n < MP_MAX_PLANES; n++) {
        int alloc_h = MP_ALIGN_UP(mpi->h, 32) >> mpi->fmt.ys[n];
        int line_bytes = (mp_image_plane_w(mpi, n) * mpi->fmt.bpp[n] + 7) / 8;
        mpi->stride[n] = FFALIGN(line_bytes, SWS_MIN_BYTE_ALIGN);
        plane_size[n] = mpi->stride[n] * alloc_h;
    }
    if (mpi->fmt.flags & MP_IMGFLAG_PAL)
        plane_size[1] = MP_PALETTE_SIZE;

    size_t sum = 0;
    for (int n = 0; n < MP_MAX_PLANES; n++)
        sum += plane_size[n];

    // Note: mp_image_pool assumes this creates only 1 AVBufferRef.
    mpi->bufs[0] = av_buffer_alloc(FFMAX(sum, 1));
    if (!mpi->bufs[0])
        return false;

    uint8_t *data = mpi->bufs[0]->data;
    for (int n = 0; n < MP_MAX_PLANES; n++) {
        mpi->planes[n] = plane_size[n] ? data : NULL;
        data += plane_size[n];
    }
    return true;
}

void mp_image_setfmt(struct mp_image *mpi, int out_fmt)
{
    struct mp_imgfmt_desc fmt = mp_imgfmt_get_desc(out_fmt);
    mpi->params.imgfmt = fmt.id;
    mpi->fmt = fmt;
    mpi->imgfmt = fmt.id;
    mpi->num_planes = fmt.num_planes;
    mp_image_set_size(mpi, mpi->w, mpi->h);
}

static void mp_image_destructor(void *ptr)
{
    mp_image_t *mpi = ptr;
    for (int p = 0; p < MP_MAX_PLANES; p++)
        av_buffer_unref(&mpi->bufs[p]);
}

int mp_chroma_div_up(int size, int shift)
{
    return (size + (1 << shift) - 1) >> shift;
}

// Return the storage width in pixels of the given plane.
int mp_image_plane_w(struct mp_image *mpi, int plane)
{
    return mp_chroma_div_up(mpi->w, mpi->fmt.xs[plane]);
}

// Return the storage height in pixels of the given plane.
int mp_image_plane_h(struct mp_image *mpi, int plane)
{
    return mp_chroma_div_up(mpi->h, mpi->fmt.ys[plane]);
}

// Caller has to make sure this doesn't exceed the allocated plane data/strides.
void mp_image_set_size(struct mp_image *mpi, int w, int h)
{
    assert(w >= 0 && h >= 0);
    mpi->w = mpi->params.w = w;
    mpi->h = mpi->params.h = h;
    mpi->params.p_w = mpi->params.p_h = 1;
}

void mp_image_set_params(struct mp_image *image,
                         const struct mp_image_params *params)
{
    // possibly initialize other stuff
    mp_image_setfmt(image, params->imgfmt);
    mp_image_set_size(image, params->w, params->h);
    image->params = *params;
}

struct mp_image *mp_image_alloc(int imgfmt, int w, int h)
{
    struct mp_image *mpi = talloc_zero(NULL, struct mp_image);
    talloc_set_destructor(mpi, mp_image_destructor);

    mp_image_set_size(mpi, w, h);
    mp_image_setfmt(mpi, imgfmt);
    if (!mp_image_alloc_planes(mpi)) {
        talloc_free(mpi);
        return NULL;
    }
    return mpi;
}

struct mp_image *mp_image_new_copy(struct mp_image *img)
{
    struct mp_image *new = mp_image_alloc(img->imgfmt, img->w, img->h);
    if (!new)
        return NULL;
    mp_image_copy(new, img);
    mp_image_copy_attributes(new, img);
    return new;
}

// Make dst take over the image data of src, and free src.
// This is basically a safe version of *dst = *src; free(src);
// Only works with ref-counted images, and can't change image size/format.
void mp_image_steal_data(struct mp_image *dst, struct mp_image *src)
{
    assert(dst->imgfmt == src->imgfmt && dst->w == src->w && dst->h == src->h);
    assert(dst->bufs[0] && src->bufs[0]);

    for (int p = 0; p < MP_MAX_PLANES; p++) {
        dst->planes[p] = src->planes[p];
        dst->stride[p] = src->stride[p];
    }
    mp_image_copy_attributes(dst, src);

    for (int p = 0; p < MP_MAX_PLANES; p++) {
        av_buffer_unref(&dst->bufs[p]);
        dst->bufs[p] = src->bufs[p];
        src->bufs[p] = NULL;
    }
    talloc_free(src);
}

// Return a new reference to img. The returned reference is owned by the caller,
// while img is left untouched.
struct mp_image *mp_image_new_ref(struct mp_image *img)
{
    if (!img)
        return NULL;

    if (!img->bufs[0])
        return mp_image_new_copy(img);

    struct mp_image *new = talloc_ptrtype(NULL, new);
    talloc_set_destructor(new, mp_image_destructor);
    *new = *img;

    bool fail = false;
    for (int p = 0; p < MP_MAX_PLANES; p++) {
        if (new->bufs[p]) {
            new->bufs[p] = av_buffer_ref(new->bufs[p]);
            if (!new->bufs[p])
                fail = true;
        }
    }

    if (!fail)
        return new;

    // Do this after _all_ bufs were changed; we don't want it to free bufs
    // from the original image if this fails.
    talloc_free(new);
    return NULL;
}

struct free_args {
    void *arg;
    void (*free)(void *arg);
};

static void call_free(void *opaque, uint8_t *data)
{
    struct free_args *args = opaque;
    args->free(args->arg);
    talloc_free(args);
}

// Create a new mp_image based on img, but don't set any buffers.
// Using this is only valid until the original img is unreferenced (including
// implicit unreferencing of the data by mp_image_make_writeable()), unless
// a new reference is set.
struct mp_image *mp_image_new_dummy_ref(struct mp_image *img)
{
    struct mp_image *new = talloc_ptrtype(NULL, new);
    talloc_set_destructor(new, mp_image_destructor);
    *new = *img;
    for (int p = 0; p < MP_MAX_PLANES; p++)
        new->bufs[p] = NULL;
    return new;
}

// Return a reference counted reference to img. If the reference count reaches
// 0, call free(free_arg). The data passed by img must not be free'd before
// that. The new reference will be writeable.
// On allocation failure, unref the frame and return NULL.
// This is only used for hw decoding; this is important, because libav* expects
// all plane data to be accounted for by AVBufferRefs.
struct mp_image *mp_image_new_custom_ref(struct mp_image *img, void *free_arg,
                                         void (*free)(void *arg))
{
    struct mp_image *new = mp_image_new_dummy_ref(img);

    struct free_args *args = talloc_ptrtype(NULL, args);
    *args = (struct free_args){free_arg, free};
    new->bufs[0] = av_buffer_create(NULL, 0, call_free, args,
                                    AV_BUFFER_FLAG_READONLY);
    if (new->bufs[0])
        return new;
    talloc_free(new);
    return NULL;
}

bool mp_image_is_writeable(struct mp_image *img)
{
    if (!img->bufs[0])
        return true; // not ref-counted => always considered writeable
    for (int p = 0; p < MP_MAX_PLANES; p++) {
        if (!img->bufs[p])
            break;
        if (!av_buffer_is_writable(img->bufs[p]))
            return false;
    }
    return true;
}

// Make the image data referenced by img writeable. This allocates new data
// if the data wasn't already writeable, and img->planes[] and img->stride[]
// will be set to the copy.
// Returns success; if false is returned, the image could not be made writeable.
bool mp_image_make_writeable(struct mp_image *img)
{
    if (mp_image_is_writeable(img))
        return true;

    struct mp_image *new = mp_image_new_copy(img);
    if (!new)
        return false;
    mp_image_steal_data(img, new);
    assert(mp_image_is_writeable(img));
    return true;
}

// Helper function: unrefs *p_img, and sets *p_img to a new ref of new_value.
// Only unrefs *p_img and sets it to NULL if out of memory.
void mp_image_setrefp(struct mp_image **p_img, struct mp_image *new_value)
{
    if (*p_img != new_value) {
        talloc_free(*p_img);
        *p_img = new_value ? mp_image_new_ref(new_value) : NULL;
    }
}

// Mere helper function (mp_image can be directly free'd with talloc_free)
void mp_image_unrefp(struct mp_image **p_img)
{
    talloc_free(*p_img);
    *p_img = NULL;
}

typedef void *(*memcpy_fn)(void *d, const void *s, size_t size);

static void memcpy_pic_cb(void *dst, const void *src, int bytesPerLine, int height,
                          int dstStride, int srcStride, memcpy_fn cpy)
{
    if (bytesPerLine == dstStride && dstStride == srcStride && height) {
        if (srcStride < 0) {
            src = (uint8_t*)src + (height - 1) * srcStride;
            dst = (uint8_t*)dst + (height - 1) * dstStride;
            srcStride = -srcStride;
        }

        cpy(dst, src, srcStride * (height - 1) + bytesPerLine);
    } else {
        for (int i = 0; i < height; i++) {
            cpy(dst, src, bytesPerLine);
            src = (uint8_t*)src + srcStride;
            dst = (uint8_t*)dst + dstStride;
        }
    }
}

static void mp_image_copy_cb(struct mp_image *dst, struct mp_image *src,
                             memcpy_fn cpy)
{
    assert(dst->imgfmt == src->imgfmt);
    assert(dst->w == src->w && dst->h == src->h);
    assert(mp_image_is_writeable(dst));
    for (int n = 0; n < dst->num_planes; n++) {
        int line_bytes = (mp_image_plane_w(dst, n) * dst->fmt.bpp[n] + 7) / 8;
        int plane_h = mp_image_plane_h(dst, n);
        memcpy_pic_cb(dst->planes[n], src->planes[n], line_bytes, plane_h,
                      dst->stride[n], src->stride[n], cpy);
    }
    // Watch out for AV_PIX_FMT_FLAG_PSEUDOPAL retardation
    if ((dst->fmt.flags & MP_IMGFLAG_PAL) && dst->planes[1] && src->planes[1])
        memcpy(dst->planes[1], src->planes[1], MP_PALETTE_SIZE);
}

void mp_image_copy(struct mp_image *dst, struct mp_image *src)
{
    mp_image_copy_cb(dst, src, memcpy);
}

void mp_image_copy_gpu(struct mp_image *dst, struct mp_image *src)
{
#if HAVE_SSE4_INTRINSICS
    if (av_get_cpu_flags() & AV_CPU_FLAG_SSE4) {
        mp_image_copy_cb(dst, src, gpu_memcpy);
        return;
    }
#endif
    mp_image_copy(dst, src);
}

// Helper, only for outputting some log info.
void mp_check_gpu_memcpy(struct mp_log *log, bool *once)
{
    if (once) {
        if (*once)
            return;
        *once = true;
    }

    bool have_sse = false;
#if HAVE_SSE4_INTRINSICS
    have_sse = av_get_cpu_flags() & AV_CPU_FLAG_SSE4;
#endif
    if (have_sse) {
        mp_verbose(log, "Using SSE4 memcpy\n");
    } else {
        mp_warn(log, "Using fallback memcpy (slow)\n");
    }
}

void mp_image_copy_attributes(struct mp_image *dst, struct mp_image *src)
{
    dst->pict_type = src->pict_type;
    dst->fields = src->fields;
    dst->pts = src->pts;
    dst->dts = src->dts;
    dst->params.rotate = src->params.rotate;
    dst->params.stereo_in = src->params.stereo_in;
    dst->params.stereo_out = src->params.stereo_out;
    if (dst->w == src->w && dst->h == src->h) {
        dst->params.p_w = src->params.p_w;
        dst->params.p_h = src->params.p_h;
    }
    dst->params.primaries = src->params.primaries;
    dst->params.gamma = src->params.gamma;
    if ((dst->fmt.flags & MP_IMGFLAG_YUV) == (src->fmt.flags & MP_IMGFLAG_YUV)) {
        dst->params.colorspace = src->params.colorspace;
        dst->params.colorlevels = src->params.colorlevels;
        dst->params.chroma_location = src->params.chroma_location;
    }
    mp_image_params_guess_csp(&dst->params); // ensure colorspace consistency
    if ((dst->fmt.flags & MP_IMGFLAG_PAL) && (src->fmt.flags & MP_IMGFLAG_PAL)) {
        if (dst->planes[1] && src->planes[1]) {
            if (mp_image_make_writeable(dst))
                memcpy(dst->planes[1], src->planes[1], MP_PALETTE_SIZE);
        }
    }
}

// Crop the given image to (x0, y0)-(x1, y1) (bottom/right border exclusive)
// x0/y0 must be naturally aligned.
void mp_image_crop(struct mp_image *img, int x0, int y0, int x1, int y1)
{
    assert(x0 >= 0 && y0 >= 0);
    assert(x0 <= x1 && y0 <= y1);
    assert(x1 <= img->w && y1 <= img->h);
    assert(!(x0 & (img->fmt.align_x - 1)));
    assert(!(y0 & (img->fmt.align_y - 1)));

    for (int p = 0; p < img->num_planes; ++p) {
        img->planes[p] += (y0 >> img->fmt.ys[p]) * img->stride[p] +
                          (x0 >> img->fmt.xs[p]) * img->fmt.bpp[p] / 8;
    }
    mp_image_set_size(img, x1 - x0, y1 - y0);
}

void mp_image_crop_rc(struct mp_image *img, struct mp_rect rc)
{
    mp_image_crop(img, rc.x0, rc.y0, rc.x1, rc.y1);
}

// Bottom/right border is allowed not to be aligned, but it might implicitly
// overwrite pixel data until the alignment (align_x/align_y) is reached.
void mp_image_clear(struct mp_image *img, int x0, int y0, int x1, int y1)
{
    assert(x0 >= 0 && y0 >= 0);
    assert(x0 <= x1 && y0 <= y1);
    assert(x1 <= img->w && y1 <= img->h);
    assert(!(x0 & (img->fmt.align_x - 1)));
    assert(!(y0 & (img->fmt.align_y - 1)));

    struct mp_image area = *img;
    mp_image_crop(&area, x0, y0, x1, y1);

    uint32_t plane_clear[MP_MAX_PLANES] = {0};

    if (area.imgfmt == IMGFMT_YUYV) {
        plane_clear[0] = av_le2ne16(0x8000);
    } else if (area.imgfmt == IMGFMT_UYVY) {
        plane_clear[0] = av_le2ne16(0x0080);
    } else if (area.imgfmt == IMGFMT_NV12 || area.imgfmt == IMGFMT_NV21) {
        plane_clear[1] = 0x8080;
    } else if (area.fmt.flags & MP_IMGFLAG_YUV_P) {
        uint16_t chroma_clear = (1 << area.fmt.plane_bits) / 2;
        if (!(area.fmt.flags & MP_IMGFLAG_NE))
            chroma_clear = av_bswap16(chroma_clear);
        if (area.num_planes > 2)
            plane_clear[1] = plane_clear[2] = chroma_clear;
    }

    for (int p = 0; p < area.num_planes; p++) {
        int bpp = area.fmt.bpp[p];
        int bytes = (mp_image_plane_w(&area, p) * bpp + 7) / 8;
        if (bpp <= 8) {
            memset_pic(area.planes[p], plane_clear[p], bytes,
                       mp_image_plane_h(&area, p), area.stride[p]);
        } else {
            memset16_pic(area.planes[p], plane_clear[p], (bytes + 1) / 2,
                         mp_image_plane_h(&area, p), area.stride[p]);
        }
    }
}

void mp_image_vflip(struct mp_image *img)
{
    for (int p = 0; p < img->num_planes; p++) {
        int plane_h = mp_image_plane_h(img, p);
        img->planes[p] = img->planes[p] + img->stride[p] * (plane_h - 1);
        img->stride[p] = -img->stride[p];
    }
}

// Display size derived from image size and pixel aspect ratio.
void mp_image_params_get_dsize(const struct mp_image_params *p,
                               int *d_w, int *d_h)
{
    *d_w = p->w;
    *d_h = p->h;
    if (p->p_w > p->p_h && p->p_h >= 1)
        *d_w = MPCLAMP(*d_w * (int64_t)p->p_w / p->p_h, 1, INT_MAX);
    if (p->p_h > p->p_w && p->p_w >= 1)
        *d_h = MPCLAMP(*d_h * (int64_t)p->p_h / p->p_w, 1, INT_MAX);
}

void mp_image_params_set_dsize(struct mp_image_params *p, int d_w, int d_h)
{
    AVRational ds = av_div_q((AVRational){d_w, d_h}, (AVRational){p->w, p->h});
    p->p_w = ds.num;
    p->p_h = ds.den;
}

char *mp_image_params_to_str_buf(char *b, size_t bs,
                                 const struct mp_image_params *p)
{
    if (p && p->imgfmt) {
        snprintf(b, bs, "%dx%d", p->w, p->h);
        if (p->p_w != p->p_h || !p->p_w)
            mp_snprintf_cat(b, bs, " [%d:%d]", p->p_w, p->p_h);
        mp_snprintf_cat(b, bs, " %s", mp_imgfmt_to_name(p->imgfmt));
        mp_snprintf_cat(b, bs, " %s/%s",
                        m_opt_choice_str(mp_csp_names, p->colorspace),
                        m_opt_choice_str(mp_csp_levels_names, p->colorlevels));
        mp_snprintf_cat(b, bs, " CL=%s",
                        m_opt_choice_str(mp_chroma_names, p->chroma_location));
        if (p->rotate)
            mp_snprintf_cat(b, bs, " rot=%d", p->rotate);
        if (p->stereo_in > 0 || p->stereo_out > 0) {
            mp_snprintf_cat(b, bs, " stereo=%s/%s",
                            MP_STEREO3D_NAME_DEF(p->stereo_in, "?"),
                            MP_STEREO3D_NAME_DEF(p->stereo_out, "?"));
        }
    } else {
        snprintf(b, bs, "???");
    }
    return b;
}

// Return whether the image parameters are valid.
// Some non-essential fields are allowed to be unset (like colorspace flags).
bool mp_image_params_valid(const struct mp_image_params *p)
{
    // av_image_check_size has similar checks and triggers around 16000*16000
    // It's mostly needed to deal with the fact that offsets are sometimes
    // ints. We also should (for now) do the same as FFmpeg, to be sure large
    // images don't crash with libswscale or when wrapping with AVFrame and
    // passing the result to filters.
    if (p->w <= 0 || p->h <= 0 || (p->w + 128LL) * (p->h + 128LL) >= INT_MAX / 8)
        return false;

    if (p->p_w <= 0 || p->p_h <= 0)
        return false;

    if (p->rotate < 0 || p->rotate >= 360)
        return false;

    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(p->imgfmt);
    if (!desc.id)
        return false;

    return true;
}

bool mp_image_params_equal(const struct mp_image_params *p1,
                           const struct mp_image_params *p2)
{
    return p1->imgfmt == p2->imgfmt &&
           p1->w == p2->w && p1->h == p2->h &&
           p1->p_w == p2->p_w && p1->p_h == p2->p_h &&
           p1->colorspace == p2->colorspace &&
           p1->colorlevels == p2->colorlevels &&
           p1->primaries == p2->primaries &&
           p1->gamma == p2->gamma &&
           p1->chroma_location == p2->chroma_location &&
           p1->rotate == p2->rotate &&
           p1->stereo_in == p2->stereo_in &&
           p1->stereo_out == p2->stereo_out;
}

// Set most image parameters, but not image format or size.
// Display size is used to set the PAR.
void mp_image_set_attributes(struct mp_image *image,
                             const struct mp_image_params *params)
{
    struct mp_image_params nparams = *params;
    nparams.imgfmt = image->imgfmt;
    nparams.w = image->w;
    nparams.h = image->h;
    if (nparams.imgfmt != params->imgfmt)
        mp_image_params_guess_csp(&nparams);
    mp_image_set_params(image, &nparams);
}

// If details like params->colorspace/colorlevels are missing, guess them from
// the other settings. Also, even if they are set, make them consistent with
// the colorspace as implied by the pixel format.
void mp_image_params_guess_csp(struct mp_image_params *params)
{
    struct mp_imgfmt_desc fmt = mp_imgfmt_get_desc(params->imgfmt);
    if (!fmt.id)
        return;
    if (fmt.flags & MP_IMGFLAG_YUV) {
        if (params->colorspace != MP_CSP_BT_601 &&
            params->colorspace != MP_CSP_BT_709 &&
            params->colorspace != MP_CSP_BT_2020_NC &&
            params->colorspace != MP_CSP_BT_2020_C &&
            params->colorspace != MP_CSP_SMPTE_240M &&
            params->colorspace != MP_CSP_YCGCO)
        {
            // Makes no sense, so guess instead
            // YCGCO should be separate, but libavcodec disagrees
            params->colorspace = MP_CSP_AUTO;
        }
        if (params->colorspace == MP_CSP_AUTO)
            params->colorspace = mp_csp_guess_colorspace(params->w, params->h);
        if (params->colorlevels == MP_CSP_LEVELS_AUTO)
            params->colorlevels = MP_CSP_LEVELS_TV;
        if (params->primaries == MP_CSP_PRIM_AUTO) {
            // Guess based on the colormatrix as a first priority
            if (params->colorspace == MP_CSP_BT_2020_NC ||
                params->colorspace == MP_CSP_BT_2020_C) {
                params->primaries = MP_CSP_PRIM_BT_2020;
            } else if (params->colorspace == MP_CSP_BT_709) {
                params->primaries = MP_CSP_PRIM_BT_709;
            } else {
                // Ambiguous colormatrix for BT.601, guess based on res
                params->primaries = mp_csp_guess_primaries(params->w, params->h);
            }
        }
        if (params->gamma == MP_CSP_TRC_AUTO)
            params->gamma = MP_CSP_TRC_BT_1886;
    } else if (fmt.flags & MP_IMGFLAG_RGB) {
        params->colorspace = MP_CSP_RGB;
        params->colorlevels = MP_CSP_LEVELS_PC;

        // The majority of RGB content is either sRGB or (rarely) some other
        // color space which we don't even handle, like AdobeRGB or
        // ProPhotoRGB. The only reasonable thing we can do is assume it's
        // sRGB and hope for the best, which should usually just work out fine.
        // Note: sRGB primaries = BT.709 primaries
        if (params->primaries == MP_CSP_PRIM_AUTO)
            params->primaries = MP_CSP_PRIM_BT_709;
        if (params->gamma == MP_CSP_TRC_AUTO)
            params->gamma = MP_CSP_TRC_SRGB;
    } else if (fmt.flags & MP_IMGFLAG_XYZ) {
        params->colorspace = MP_CSP_XYZ;
        params->colorlevels = MP_CSP_LEVELS_PC;

        // The default XYZ matrix converts it to BT.709 color space
        // since that's the most likely scenario. Proper VOs should ignore
        // this field as well as the matrix and treat XYZ input as absolute,
        // but for VOs which use the matrix (and hence, consult this field)
        // this is the correct parameter. This doubles as a reasonable output
        // gamut for VOs which *do* use the specialized XYZ matrix but don't
        // know any better output gamut other than whatever the source is
        // tagged with.
        if (params->primaries == MP_CSP_PRIM_AUTO)
            params->primaries = MP_CSP_PRIM_BT_709;
        if (params->gamma == MP_CSP_TRC_AUTO)
            params->gamma = MP_CSP_TRC_LINEAR;
    } else {
        // We have no clue.
        params->colorspace = MP_CSP_AUTO;
        params->colorlevels = MP_CSP_LEVELS_AUTO;
        params->primaries = MP_CSP_PRIM_AUTO;
        params->gamma = MP_CSP_TRC_AUTO;
    }
}

// Copy properties and data of the AVFrame into the mp_image, without taking
// care of memory management issues.
void mp_image_copy_fields_from_av_frame(struct mp_image *dst,
                                        struct AVFrame *src)
{
    mp_image_setfmt(dst, pixfmt2imgfmt(src->format));
    mp_image_set_size(dst, src->width, src->height);

    for (int i = 0; i < 4; i++) {
        dst->planes[i] = src->data[i];
        dst->stride[i] = src->linesize[i];
    }

    dst->pict_type = src->pict_type;

    dst->fields = 0;
    if (src->interlaced_frame)
        dst->fields |= MP_IMGFIELD_INTERLACED;
    if (src->top_field_first)
        dst->fields |= MP_IMGFIELD_TOP_FIRST;
    if (src->repeat_pict == 1)
        dst->fields |= MP_IMGFIELD_REPEAT_FIRST;
}

// Copy properties and data of the mp_image into the AVFrame, without taking
// care of memory management issues.
void mp_image_copy_fields_to_av_frame(struct AVFrame *dst,
                                      struct mp_image *src)
{
    dst->format = imgfmt2pixfmt(src->imgfmt);
    dst->width = src->w;
    dst->height = src->h;

    for (int i = 0; i < 4; i++) {
        dst->data[i] = src->planes[i];
        dst->linesize[i] = src->stride[i];
    }
    dst->extended_data = dst->data;

    dst->pict_type = src->pict_type;
    if (src->fields & MP_IMGFIELD_INTERLACED)
        dst->interlaced_frame = 1;
    if (src->fields & MP_IMGFIELD_TOP_FIRST)
        dst->top_field_first = 1;
    if (src->fields & MP_IMGFIELD_REPEAT_FIRST)
        dst->repeat_pict = 1;

    dst->colorspace = mp_csp_to_avcol_spc(src->params.colorspace);
    dst->color_range = mp_csp_levels_to_avcol_range(src->params.colorlevels);
}

// Create a new mp_image reference to av_frame.
struct mp_image *mp_image_from_av_frame(struct AVFrame *av_frame)
{
    struct mp_image t = {0};
    mp_image_copy_fields_from_av_frame(&t, av_frame);
    for (int p = 0; p < MP_MAX_PLANES; p++)
        t.bufs[p] = av_frame->buf[p];
    return mp_image_new_ref(&t);
}

// Convert the mp_image reference to a AVFrame reference.
// Warning: img is unreferenced (i.e. free'd). This is asymmetric to
//          mp_image_from_av_frame(). It was done as some sort of optimization,
//          but now these semantics are pointless.
// On failure, img is only unreffed.
struct AVFrame *mp_image_to_av_frame_and_unref(struct mp_image *img)
{
    struct mp_image *new_ref = mp_image_new_ref(img); // ensure it's refcounted
    talloc_free(img);
    if (!new_ref)
        return NULL;
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        talloc_free(new_ref);
        return NULL;
    }
    mp_image_copy_fields_to_av_frame(frame, new_ref);
    for (int p = 0; p < MP_MAX_PLANES; p++) {
        frame->buf[p] = new_ref->bufs[p];
        new_ref->bufs[p] = NULL;
    }
    talloc_free(new_ref);
    return frame;
}

void memcpy_pic(void *dst, const void *src, int bytesPerLine, int height,
                int dstStride, int srcStride)
{
    memcpy_pic_cb(dst, src, bytesPerLine, height, dstStride, srcStride, memcpy);
}

void memset_pic(void *dst, int fill, int bytesPerLine, int height, int stride)
{
    if (bytesPerLine == stride && height) {
        memset(dst, fill, stride * (height - 1) + bytesPerLine);
    } else {
        for (int i = 0; i < height; i++) {
            memset(dst, fill, bytesPerLine);
            dst = (uint8_t *)dst + stride;
        }
    }
}

void memset16_pic(void *dst, int fill, int unitsPerLine, int height, int stride)
{
    if (fill == 0) {
        memset_pic(dst, 0, unitsPerLine * 2, height, stride);
    } else {
        for (int i = 0; i < height; i++) {
            uint16_t *line = dst;
            uint16_t *end = line + unitsPerLine;
            while (line < end)
                *line++ = fill;
            dst = (uint8_t *)dst + stride;
        }
    }
}
