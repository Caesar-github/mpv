/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>

#include <libavutil/common.h>
#include <libavutil/opt.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixdesc.h>

#include "compat/libav.h"

#include "talloc.h"
#include "config.h"
#include "common/msg.h"
#include "options/options.h"
#include "bstr/bstr.h"
#include "common/av_opts.h"
#include "common/av_common.h"
#include "common/codecs.h"

#include "compat/mpbswap.h"
#include "video/fmt-conversion.h"

#include "vd.h"
#include "video/img_format.h"
#include "video/mp_image_pool.h"
#include "video/filter/vf.h"
#include "video/decode/dec_video.h"
#include "demux/stheader.h"
#include "demux/packet.h"
#include "video/csputils.h"

#include "lavc.h"

#if AVPALETTE_SIZE != MP_PALETTE_SIZE
#error palette too large, adapt video/mp_image.h:MP_PALETTE_SIZE
#endif

#include "options/m_option.h"

static void init_avctx(struct dec_video *vd, const char *decoder,
                       struct vd_lavc_hwdec *hwdec);
static void uninit_avctx(struct dec_video *vd);
static void setup_refcounting_hw(struct AVCodecContext *s);

static enum AVPixelFormat get_format_hwdec(struct AVCodecContext *avctx,
                                           const enum AVPixelFormat *pix_fmt);

static void uninit(struct dec_video *vd);

#define OPT_BASE_STRUCT struct MPOpts

const m_option_t lavc_decode_opts_conf[] = {
    OPT_FLAG_CONSTANTS("fast", lavc_param.fast, 0, 0, CODEC_FLAG2_FAST),
    OPT_FLAG("show-all", lavc_param.show_all, 0),
    OPT_STRING("skiploopfilter", lavc_param.skip_loop_filter_str, 0),
    OPT_STRING("skipidct", lavc_param.skip_idct_str, 0),
    OPT_STRING("skipframe", lavc_param.skip_frame_str, 0),
    OPT_INTRANGE("threads", lavc_param.threads, 0, 0, 16),
    OPT_FLAG_CONSTANTS("bitexact", lavc_param.bitexact, 0, 0, CODEC_FLAG_BITEXACT),
    OPT_FLAG("check-hw-profile", lavc_param.check_hw_profile, 0),
    OPT_STRING("o", lavc_param.avopt, 0),
    {NULL, NULL, 0, 0, 0, 0, NULL}
};

const struct vd_lavc_hwdec mp_vd_lavc_vdpau;
const struct vd_lavc_hwdec mp_vd_lavc_vdpau_old;
const struct vd_lavc_hwdec mp_vd_lavc_vda;
const struct vd_lavc_hwdec mp_vd_lavc_vaapi;
const struct vd_lavc_hwdec mp_vd_lavc_vaapi_copy;

static const struct vd_lavc_hwdec *hwdec_list[] = {
#if HAVE_VDPAU_HWACCEL
    &mp_vd_lavc_vdpau,
#endif
#if HAVE_VDPAU_DECODER
    &mp_vd_lavc_vdpau_old,
#endif
#if HAVE_VDA_HWACCEL
    &mp_vd_lavc_vda,
#endif
#if HAVE_VAAPI_HWACCEL
    &mp_vd_lavc_vaapi,
    &mp_vd_lavc_vaapi_copy,
#endif
    NULL
};

static struct vd_lavc_hwdec *find_hwcodec(enum hwdec_type api)
{
    for (int n = 0; hwdec_list[n]; n++) {
        if (hwdec_list[n]->type == api)
            return (struct vd_lavc_hwdec *)hwdec_list[n];
    }
    return NULL;
}

static bool hwdec_codec_allowed(struct dec_video *vd, const char *codec)
{
    bstr s = bstr0(vd->opts->hwdec_codecs);
    while (s.len) {
        bstr item;
        bstr_split_tok(s, ",", &item, &s);
        if (bstr_equals0(item, "all") || bstr_equals0(item, codec))
            return true;
    }
    return false;
}

static enum AVDiscard str2AVDiscard(struct dec_video *vd, char *str)
{
    if (!str)                               return AVDISCARD_DEFAULT;
    if (strcasecmp(str, "none"   ) == 0)    return AVDISCARD_NONE;
    if (strcasecmp(str, "default") == 0)    return AVDISCARD_DEFAULT;
    if (strcasecmp(str, "nonref" ) == 0)    return AVDISCARD_NONREF;
    if (strcasecmp(str, "bidir"  ) == 0)    return AVDISCARD_BIDIR;
    if (strcasecmp(str, "nonkey" ) == 0)    return AVDISCARD_NONKEY;
    if (strcasecmp(str, "all"    ) == 0)    return AVDISCARD_ALL;
    MP_ERR(vd, "Unknown discard value %s\n", str);
    return AVDISCARD_DEFAULT;
}

// Find the correct profile entry for the current codec and profile.
// Assumes the table has higher profiles first (for each codec).
const struct hwdec_profile_entry *hwdec_find_profile(
    struct lavc_ctx *ctx, const struct hwdec_profile_entry *table)
{
    assert(AV_CODEC_ID_NONE == 0);
    struct lavc_param *lavc_param = &ctx->opts->lavc_param;
    enum AVCodecID codec = ctx->avctx->codec_id;
    int profile = ctx->avctx->profile;
    // Assume nobody cares about these aspects of the profile
    if (codec == AV_CODEC_ID_H264) {
        if (profile == FF_PROFILE_H264_CONSTRAINED_BASELINE)
            profile = FF_PROFILE_H264_MAIN;
    }
    for (int n = 0; table[n].av_codec; n++) {
        if (table[n].av_codec == codec) {
            if (table[n].ff_profile == FF_PROFILE_UNKNOWN ||
                profile == FF_PROFILE_UNKNOWN ||
                table[n].ff_profile == profile ||
                !lavc_param->check_hw_profile)
                return &table[n];
        }
    }
    return NULL;
}

// Check codec support, without checking the profile.
bool hwdec_check_codec_support(const char *decoder,
                               const struct hwdec_profile_entry *table)
{
    enum AVCodecID codec = mp_codec_to_av_codec_id(decoder);
    for (int n = 0; table[n].av_codec; n++) {
        if (table[n].av_codec == codec)
            return true;
    }
    return false;
}

int hwdec_get_max_refs(struct lavc_ctx *ctx)
{
    return ctx->avctx->codec_id == AV_CODEC_ID_H264 ? 16 : 2;
}

void hwdec_request_api(struct mp_hwdec_info *info, const char *api_name)
{
    if (info && info->load_api)
        info->load_api(info, api_name);
}

static int hwdec_probe(struct vd_lavc_hwdec *hwdec, struct mp_hwdec_info *info,
                       const char *decoder, const char **hw_decoder)
{
    if (hwdec->codec_pairs) {
        for (int n = 0; hwdec->codec_pairs[n + 0]; n += 2) {
            const char *sw = hwdec->codec_pairs[n + 0];
            const char *hw = hwdec->codec_pairs[n + 1];
            if (decoder && strcmp(decoder, sw) == 0) {
                AVCodec *codec = avcodec_find_decoder_by_name(hw);
                *hw_decoder = hw;
                if (codec)
                    goto found;
            }
        }
        return HWDEC_ERR_NO_CODEC;
    found: ;
    }
    int r = 0;
    if (hwdec->probe)
        r = hwdec->probe(hwdec, info, decoder);
    return r;
}

static bool probe_hwdec(struct dec_video *vd, bool autoprobe, enum hwdec_type api,
                        const char *decoder, struct vd_lavc_hwdec **use_hwdec,
                        const char **use_decoder)
{
    struct vd_lavc_hwdec *hwdec = find_hwcodec(api);
    if (!hwdec) {
        MP_VERBOSE(vd, "Requested hardware decoder not "
                   "compiled.\n");
        return false;
    }
    const char *hw_decoder = NULL;
    int r = hwdec_probe(hwdec, &vd->hwdec_info, decoder, &hw_decoder);
    if (r >= 0) {
        *use_hwdec = hwdec;
        *use_decoder = hw_decoder;
        return true;
    } else if (r == HWDEC_ERR_NO_CODEC) {
        MP_VERBOSE(vd, "Hardware decoder '%s' not found in "
                   "libavcodec.\n", hw_decoder ? hw_decoder : decoder);
    } else if (r == HWDEC_ERR_NO_CTX && !autoprobe) {
        MP_WARN(vd, "VO does not support requested "
                "hardware decoder.\n");
    }
    return false;
}


static int init(struct dec_video *vd, const char *decoder)
{
    vd_ffmpeg_ctx *ctx;
    ctx = vd->priv = talloc_zero(NULL, vd_ffmpeg_ctx);
    ctx->log = vd->log;
    ctx->opts = vd->opts;
    ctx->non_dr1_pool = talloc_steal(ctx, mp_image_pool_new(16));

    if (bstr_endswith0(bstr0(decoder), "_vdpau")) {
        MP_WARN(vd, "VDPAU decoder '%s' was requested. "
                "This way of enabling hardware\ndecoding is not supported "
                "anymore. Use --hwdec=vdpau instead.\nThe --hwdec-codec=... "
                "option can be used to restrict which codecs are\nenabled, "
                "otherwise all hardware decoding is tried for all codecs.\n",
                decoder);
        uninit(vd);
        return 0;
    }

    struct vd_lavc_hwdec *hwdec = NULL;
    const char *hw_decoder = NULL;

    if (hwdec_codec_allowed(vd, decoder)) {
        if (vd->opts->hwdec_api == HWDEC_AUTO) {
            for (int n = 0; hwdec_list[n]; n++) {
                if (probe_hwdec(vd, true, hwdec_list[n]->type, decoder,
                    &hwdec, &hw_decoder))
                    break;
            }
        } else if (vd->opts->hwdec_api != HWDEC_NONE) {
            probe_hwdec(vd, false, vd->opts->hwdec_api, decoder,
                        &hwdec, &hw_decoder);
        }
    } else {
        MP_VERBOSE(vd, "Not trying to use hardware decoding: "
                   "codec %s is blacklisted by user.\n", decoder);
    }

    if (hwdec) {
        ctx->software_fallback_decoder = talloc_strdup(ctx, decoder);
        if (hw_decoder)
            decoder = hw_decoder;
        MP_INFO(vd, "Trying to use hardware decoding.\n");
    } else if (vd->opts->hwdec_api != HWDEC_NONE) {
        MP_INFO(vd, "Using software decoding.\n");
    }

    init_avctx(vd, decoder, hwdec);
    if (!ctx->avctx) {
        if (ctx->software_fallback_decoder) {
            MP_ERR(vd, "Error initializing hardware decoding, "
                   "falling back to software decoding.\n");
            decoder = ctx->software_fallback_decoder;
            ctx->software_fallback_decoder = NULL;
            init_avctx(vd, decoder, NULL);
        }
        if (!ctx->avctx) {
            uninit(vd);
            return 0;
        }
    }
    return 1;
}

static void set_from_bih(AVCodecContext *avctx, uint32_t format,
                         MP_BITMAPINFOHEADER *bih)
{

    switch (format) {
    case MP_FOURCC('S','V','Q','3'):
    case MP_FOURCC('A','V','R','n'):
    case MP_FOURCC('M','J','P','G'):
        /* AVRn stores huffman table in AVI header */
        /* Pegasus MJPEG stores it also in AVI header, but it uses the common
         * MJPG fourcc :( */
        if (bih->biSize <= sizeof(*bih))
           break;
        av_opt_set_int(avctx, "extern_huff", 1, AV_OPT_SEARCH_CHILDREN);
        mp_lavc_set_extradata(avctx, bih + 1, bih->biSize - sizeof(*bih));
        break;

    case MP_FOURCC('R','V','1','0'):
    case MP_FOURCC('R','V','1','3'):
    case MP_FOURCC('R','V','2','0'):
    case MP_FOURCC('R','V','3','0'):
    case MP_FOURCC('R','V','4','0'):
        if (bih->biSize < sizeof(*bih) + 8) {
            // only 1 packet per frame & sub_id from fourcc
            uint32_t extradata[2] = {
                0,
                format == MP_FOURCC('R','V','1','3') ? 0x10003001 : 0x10000000,
            };
            mp_lavc_set_extradata(avctx, &extradata, 8);
        } else {
            // has extra slice header (demux_rm or rm->avi streamcopy)
            mp_lavc_set_extradata(avctx, bih + 1, bih->biSize - sizeof(*bih));
        }
        break;

    default:
        if (bih->biSize <= sizeof(*bih))
            break;
        mp_lavc_set_extradata(avctx, bih + 1, bih->biSize - sizeof(*bih));
        break;
    }

    avctx->bits_per_coded_sample = bih->biBitCount;
    avctx->coded_width  = bih->biWidth;
    avctx->coded_height = bih->biHeight;
}

static void init_avctx(struct dec_video *vd, const char *decoder,
                       struct vd_lavc_hwdec *hwdec)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    struct lavc_param *lavc_param = &vd->opts->lavc_param;
    bool mp_rawvideo = false;
    struct sh_stream *sh = vd->header;

    assert(!ctx->avctx);

    if (strcmp(decoder, "mp-rawvideo") == 0) {
        mp_rawvideo = true;
        decoder = "rawvideo";
    }

    AVCodec *lavc_codec = avcodec_find_decoder_by_name(decoder);
    if (!lavc_codec)
        return;

    ctx->hwdec_info = &vd->hwdec_info;

    ctx->do_dr1 = ctx->do_hw_dr1 = 0;
    ctx->pix_fmt = AV_PIX_FMT_NONE;
    ctx->hwdec = hwdec;
    ctx->avctx = avcodec_alloc_context3(lavc_codec);
    AVCodecContext *avctx = ctx->avctx;
    avctx->opaque = vd;
    avctx->codec_type = AVMEDIA_TYPE_VIDEO;
    avctx->codec_id = lavc_codec->id;

#if HAVE_AVUTIL_REFCOUNTING
    avctx->refcounted_frames = 1;
    ctx->pic = av_frame_alloc();
#else
    ctx->pic = avcodec_alloc_frame();
#endif

    if (ctx->hwdec) {
        ctx->do_hw_dr1         = true;
        avctx->thread_count    = 1;
        avctx->get_format      = get_format_hwdec;
        setup_refcounting_hw(avctx);
        if (ctx->hwdec->init(ctx) < 0) {
            uninit_avctx(vd);
            return;
        }
    } else {
#if !HAVE_AVUTIL_REFCOUNTING
        if (lavc_codec->capabilities & CODEC_CAP_DR1) {
            ctx->do_dr1            = true;
            avctx->get_buffer      = mp_codec_get_buffer;
            avctx->release_buffer  = mp_codec_release_buffer;
        }
#endif
        mp_set_avcodec_threads(avctx, lavc_param->threads);
    }

    avctx->flags |= lavc_param->bitexact;

    avctx->flags2 |= lavc_param->fast;
    if (lavc_param->show_all) {
#ifdef CODEC_FLAG2_SHOW_ALL
        avctx->flags2 |= CODEC_FLAG2_SHOW_ALL; // ffmpeg only?
#endif
#ifdef CODEC_FLAG_OUTPUT_CORRUPT
        avctx->flags |= CODEC_FLAG_OUTPUT_CORRUPT; // added with Libav 10
#endif
    }

    avctx->skip_loop_filter = str2AVDiscard(vd, lavc_param->skip_loop_filter_str);
    avctx->skip_idct = str2AVDiscard(vd, lavc_param->skip_idct_str);
    avctx->skip_frame = str2AVDiscard(vd, lavc_param->skip_frame_str);

    if (lavc_param->avopt) {
        if (parse_avopts(avctx, lavc_param->avopt) < 0) {
            MP_ERR(vd, "Your options /%s/ look like gibberish to me pal\n",
                   lavc_param->avopt);
            uninit_avctx(vd);
            return;
        }
    }

    // Do this after the above avopt handling in case it changes values
    ctx->skip_frame = avctx->skip_frame;

    avctx->codec_tag = sh->format;
    avctx->coded_width  = sh->video->disp_w;
    avctx->coded_height = sh->video->disp_h;

    // demux_mkv
    if (sh->video->bih)
        set_from_bih(avctx, sh->format, sh->video->bih);

    if (mp_rawvideo) {
        avctx->pix_fmt = imgfmt2pixfmt(sh->format);
        avctx->codec_tag = 0;
        if (avctx->pix_fmt == AV_PIX_FMT_NONE && sh->format)
            MP_ERR(vd, "Image format %s not supported by lavc.\n",
                   mp_imgfmt_to_name(sh->format));
    }

    if (sh->lav_headers)
        mp_copy_lav_codec_headers(avctx, sh->lav_headers);

    /* open it */
    if (avcodec_open2(avctx, lavc_codec, NULL) < 0) {
        MP_ERR(vd, "Could not open codec.\n");
        uninit_avctx(vd);
        return;
    }
}

static void uninit_avctx(struct dec_video *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVCodecContext *avctx = ctx->avctx;

    if (avctx) {
        if (avctx->codec && avcodec_close(avctx) < 0)
            MP_ERR(vd, "Could not close codec.\n");

        av_freep(&avctx->extradata);
        av_freep(&avctx->slice_offset);
    }

    av_freep(&ctx->avctx);

    if (ctx->hwdec && ctx->hwdec->uninit)
        ctx->hwdec->uninit(ctx);

#if HAVE_AVUTIL_REFCOUNTING
    av_frame_free(&ctx->pic);
#else
    avcodec_free_frame(&ctx->pic);
    mp_buffer_pool_free(&ctx->dr1_buffer_pool);
#endif
}

static void uninit(struct dec_video *vd)
{
    uninit_avctx(vd);
}

static void update_image_params(struct dec_video *vd, AVFrame *frame,
                                struct mp_image_params *out_params)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    int width = frame->width;
    int height = frame->height;
    float aspect = av_q2d(frame->sample_aspect_ratio) * width / height;
    int pix_fmt = frame->format;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(54, 40, 0)
    pix_fmt = ctx->avctx->pix_fmt;
#endif

    if (pix_fmt != ctx->pix_fmt) {
        ctx->pix_fmt = pix_fmt;
        ctx->best_csp = pixfmt2imgfmt(pix_fmt);
        if (!ctx->best_csp)
            MP_ERR(vd, "lavc pixel format %s not supported.\n",
                   av_get_pix_fmt_name(pix_fmt));
    }

    int d_w, d_h;
    vf_set_dar(&d_w, &d_h, width, height, aspect);

    *out_params = (struct mp_image_params) {
        .imgfmt = ctx->best_csp,
        .w = width,
        .h = height,
        .d_w = d_w,
        .d_h = d_h,
        .colorspace = avcol_spc_to_mp_csp(ctx->avctx->colorspace),
        .colorlevels = avcol_range_to_mp_csp_levels(ctx->avctx->color_range),
        .chroma_location =
            avchroma_location_to_mp(ctx->avctx->chroma_sample_location),
    };
}

static enum AVPixelFormat get_format_hwdec(struct AVCodecContext *avctx,
                                           const enum AVPixelFormat *fmt)
{
    struct dec_video *vd = avctx->opaque;
    vd_ffmpeg_ctx *ctx = vd->priv;

    MP_VERBOSE(vd, "Pixel formats supported by decoder:");
    for (int i = 0; fmt[i] != AV_PIX_FMT_NONE; i++)
        MP_VERBOSE(vd, " %s", av_get_pix_fmt_name(fmt[i]));
    MP_VERBOSE(vd, "\n");

    assert(ctx->hwdec);

    for (int i = 0; fmt[i] != AV_PIX_FMT_NONE; i++) {
        const int *okfmt = ctx->hwdec->image_formats;
        for (int n = 0; okfmt && okfmt[n]; n++) {
            if (imgfmt2pixfmt(okfmt[n]) == fmt[i])
                return fmt[i];
        }
    }

    return AV_PIX_FMT_NONE;
}

static struct mp_image *get_surface_hwdec(struct dec_video *vd, AVFrame *pic)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    /* Decoders using ffmpeg's hwaccel architecture (everything except vdpau)
     * can fall back to software decoding automatically. However, we don't
     * want that: multithreading was already disabled. ffmpeg's fallback
     * isn't really useful, and causes more trouble than it helps.
     *
     * Instead of trying to "adjust" the thread_count fields in avctx, let
     * decoding fail hard. Then decode_with_fallback() will do our own software
     * fallback. Fully reinitializing the decoder is saner, and will probably
     * save us from other weird corner cases, like having to "reroute" the
     * get_buffer callback.
     */
    int imgfmt = pixfmt2imgfmt(pic->format);
    if (!IMGFMT_IS_HWACCEL(imgfmt))
        return NULL;

    // Using frame->width/height is bad. For non-mod 16 video (which would
    // require alignment of frame sizes) we want the decoded size, not the
    // aligned size. At least vdpau needs this: the video mixer is created
    // with decoded size, and the video surfaces must have matching size.
    int w = ctx->avctx->width;
    int h = ctx->avctx->height;

    struct mp_image *mpi = ctx->hwdec->allocate_image(ctx, imgfmt, w, h);

    if (mpi) {
        for (int i = 0; i < 4; i++)
            pic->data[i] = mpi->planes[i];
    }

    return mpi;
}

#if HAVE_AVUTIL_REFCOUNTING

static void free_mpi(void *opaque, uint8_t *data)
{
    struct mp_image *mpi = opaque;
    talloc_free(mpi);
}

static int get_buffer2_hwdec(AVCodecContext *avctx, AVFrame *pic, int flags)
{
    struct dec_video *vd = avctx->opaque;

    struct mp_image *mpi = get_surface_hwdec(vd, pic);
    if (!mpi)
        return -1;

    pic->buf[0] = av_buffer_create(NULL, 0, free_mpi, mpi, 0);

    return 0;
}

static void setup_refcounting_hw(AVCodecContext *avctx)
{
    avctx->get_buffer2 = get_buffer2_hwdec;
}

#else /* HAVE_AVUTIL_REFCOUNTING */

static int get_buffer_hwdec(AVCodecContext *avctx, AVFrame *pic)
{
    struct dec_video *vd = avctx->opaque;

    struct mp_image *mpi = get_surface_hwdec(vd, pic);
    if (!mpi)
        return -1;

    pic->opaque = mpi;
    pic->type = FF_BUFFER_TYPE_USER;

    /* The libavcodec reordered_opaque functionality is implemented by
     * a similar copy in avcodec_default_get_buffer() and without a
     * workaround like this it'd stop working when a custom buffer
     * callback is used.
     */
    pic->reordered_opaque = avctx->reordered_opaque;
    return 0;
}

static void release_buffer_hwdec(AVCodecContext *avctx, AVFrame *pic)
{
    mp_image_t *mpi = pic->opaque;

    assert(pic->type == FF_BUFFER_TYPE_USER);
    assert(mpi);

    talloc_free(mpi);

    for (int i = 0; i < 4; i++)
        pic->data[i] = NULL;
}

static void setup_refcounting_hw(AVCodecContext *avctx)
{
    avctx->get_buffer = get_buffer_hwdec;
    avctx->release_buffer = release_buffer_hwdec;
}

#endif /* HAVE_AVUTIL_REFCOUNTING */

#if HAVE_AVUTIL_REFCOUNTING

static struct mp_image *image_from_decoder(struct dec_video *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVFrame *pic = ctx->pic;

    struct mp_image *img = mp_image_from_av_frame(pic);
    av_frame_unref(pic);

    return img;
}

#else /* HAVE_AVUTIL_REFCOUNTING */

static void fb_ref(void *b)
{
    mp_buffer_ref(b);
}

static void fb_unref(void *b)
{
    mp_buffer_unref(b);
}

static bool fb_is_unique(void *b)
{
    return mp_buffer_is_unique(b);
}

static struct mp_image *image_from_decoder(struct dec_video *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVFrame *pic = ctx->pic;

    struct mp_image new = {0};
    mp_image_copy_fields_from_av_frame(&new, pic);

    struct mp_image *mpi;
    if (ctx->do_hw_dr1 && pic->opaque) {
        mpi = pic->opaque; // reordered frame
        assert(mpi);
        mpi = mp_image_new_ref(mpi);
        mp_image_copy_attributes(mpi, &new);
    } else if (ctx->do_dr1 && pic->opaque) {
        struct FrameBuffer *fb = pic->opaque;
        // initial reference for mpi
        if (!new.planes[0] || !mp_buffer_check(fb)) {
            // Decoder returned an unreferenced buffer! Taking this would just
            // lead to an eventual double-free. Nothing we can do about this.
            // So just say "fuck you" in a nice way.
            MP_FATAL(vd,
    "Impossible condition detected! This version of Libav/FFmpeg is not\n"
    "supported anymore. Please update.\n");
            return NULL;
        }
        mpi = mp_image_new_external_ref(&new, fb, fb_ref, fb_unref,
                                        fb_is_unique, NULL);
    } else {
        mpi = mp_image_pool_new_copy(ctx->non_dr1_pool, &new);
    }
    return mpi;
}

#endif /* HAVE_AVUTIL_REFCOUNTING */

static int decode(struct dec_video *vd, struct demux_packet *packet,
                  int flags, struct mp_image **out_image)
{
    int got_picture = 0;
    int ret;
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVCodecContext *avctx = ctx->avctx;
    AVPacket pkt;

    if (flags & 2)
        avctx->skip_frame = AVDISCARD_ALL;
    else if (flags & 1)
        avctx->skip_frame = AVDISCARD_NONREF;
    else
        avctx->skip_frame = ctx->skip_frame;

    mp_set_av_packet(&pkt, packet, NULL);

    ret = avcodec_decode_video2(avctx, ctx->pic, &got_picture, &pkt);
    if (ret < 0) {
        MP_WARN(vd, "Error while decoding frame!\n");
        return -1;
    }

    // Skipped frame, or delayed output due to multithreaded decoding.
    if (!got_picture)
        return 0;

    struct mp_image_params params;
    update_image_params(vd, ctx->pic, &params);
    vd->codec_pts = mp_pts_from_av(ctx->pic->pkt_pts, NULL);
    vd->codec_dts = mp_pts_from_av(ctx->pic->pkt_dts, NULL);

    // Note: potentially resets ctx->pic as it is transferred to mpi
    struct mp_image *mpi = image_from_decoder(vd);
    if (!mpi)
        return 0;
    assert(mpi->planes[0]);
    mp_image_set_params(mpi, &params);

    if (ctx->hwdec && ctx->hwdec->process_image)
        mpi = ctx->hwdec->process_image(ctx, mpi);

    *out_image = mpi;
    return 1;
}

static int force_fallback(struct dec_video *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    if (ctx->software_fallback_decoder) {
        uninit_avctx(vd);
        MP_ERR(vd, "Error using hardware "
                "decoding, falling back to software decoding.\n");
        const char *decoder = ctx->software_fallback_decoder;
        ctx->software_fallback_decoder = NULL;
        init_avctx(vd, decoder, NULL);
        return ctx->avctx ? CONTROL_OK : CONTROL_ERROR;
    }
    return CONTROL_FALSE;
}

static struct mp_image *decode_with_fallback(struct dec_video *vd,
                                struct demux_packet *packet, int flags)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    if (!ctx->avctx)
        return NULL;

    struct mp_image *mpi = NULL;
    int res = decode(vd, packet, flags, &mpi);
    if (res < 0) {
        // Failed hardware decoding? Try again in software.
        if (force_fallback(vd) == CONTROL_OK)
            decode(vd, packet, flags, &mpi);
    }

    return mpi;
}

static int control(struct dec_video *vd, int cmd, void *arg)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVCodecContext *avctx = ctx->avctx;
    switch (cmd) {
    case VDCTRL_RESET:
        avcodec_flush_buffers(avctx);
        return CONTROL_TRUE;
    case VDCTRL_QUERY_UNSEEN_FRAMES:;
        int delay = avctx->has_b_frames;
        assert(delay >= 0);
        if (avctx->active_thread_type & FF_THREAD_FRAME)
            delay += avctx->thread_count - 1;
        *(int *)arg = delay;
        return CONTROL_TRUE;
    case VDCTRL_FORCE_HWDEC_FALLBACK:
        return force_fallback(vd);
    }
    return CONTROL_UNKNOWN;
}

static void add_decoders(struct mp_decoder_list *list)
{
    mp_add_lavc_decoders(list, AVMEDIA_TYPE_VIDEO);
    mp_add_decoder(list, "lavc", "mp-rawvideo", "mp-rawvideo",
                   "raw video");
}

const struct vd_functions mpcodecs_vd_ffmpeg = {
    .name = "lavc",
    .add_decoders = add_decoders,
    .init = init,
    .uninit = uninit,
    .control = control,
    .decode = decode_with_fallback,
};
