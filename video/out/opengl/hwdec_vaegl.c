/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <string.h>
#include <assert.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <va/va_drmcommon.h>

#include "hwdec.h"
#include "video/vaapi.h"
#include "video/img_fourcc.h"
#include "video/mp_image_pool.h"
#include "common.h"

#ifndef GL_OES_EGL_image
typedef void* GLeglImageOES;
#endif
#ifndef EGL_KHR_image
typedef void *EGLImageKHR;
#endif

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT             0x3270
#define EGL_LINUX_DRM_FOURCC_EXT          0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT         0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT     0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT      0x3274
#endif

#if HAVE_VAAPI_X11
#include <va/va_x11.h>

static VADisplay *create_x11_va_display(GL *gl)
{
    Display *x11 = gl->MPGetNativeDisplay("x11");
    return x11 ? vaGetDisplay(x11) : NULL;
}
#endif

#if HAVE_VAAPI_WAYLAND
#include <va/va_wayland.h>

static VADisplay *create_wayland_va_display(GL *gl)
{
    struct wl_display *wl = gl->MPGetNativeDisplay("wl");
    return wl ? vaGetDisplayWl(wl) : NULL;
}
#endif

#if HAVE_VAAPI_DRM
#include <va/va_drm.h>

static VADisplay *create_drm_va_display(GL *gl)
{
    int drm_fd = (intptr_t)gl->MPGetNativeDisplay("drm");
    // Note: yes, drm_fd==0 could be valid - but it's rare and doesn't fit with
    //       our slightly crappy way of passing it through, so consider 0 not
    //       valid.
    return drm_fd ? vaGetDisplayDRM(drm_fd) : NULL;
}
#endif

struct va_create_native {
    VADisplay *(*create)(GL *gl);
};

static const struct va_create_native create_native_cbs[] = {
#if HAVE_VAAPI_X11
    {create_x11_va_display},
#endif
#if HAVE_VAAPI_WAYLAND
    {create_wayland_va_display},
#endif
#if HAVE_VAAPI_DRM
    {create_drm_va_display},
#endif
};

static VADisplay *create_native_va_display(GL *gl)
{
    if (!gl->MPGetNativeDisplay)
        return NULL;
    for (int n = 0; n < MP_ARRAY_SIZE(create_native_cbs); n++) {
        VADisplay *display = create_native_cbs[n].create(gl);
        if (display)
            return display;
    }
    return NULL;
}

struct priv {
    struct mp_log *log;
    struct mp_vaapi_ctx *ctx;
    VADisplay *display;
    GLuint gl_textures[4];
    EGLImageKHR images[4];
    VAImage current_image;
    bool buffer_acquired;
    int current_mpfmt;

    EGLImageKHR (EGLAPIENTRY *CreateImageKHR)(EGLDisplay, EGLContext,
                                              EGLenum, EGLClientBuffer,
                                              const EGLint *);
    EGLBoolean (EGLAPIENTRY *DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    void (EGLAPIENTRY *EGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);
};

static bool test_format(struct gl_hwdec *hw);

static void unmap_frame(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    VAStatus status;

    for (int n = 0; n < 4; n++) {
        if (p->images[n])
            p->DestroyImageKHR(eglGetCurrentDisplay(), p->images[n]);
        p->images[n] = 0;
    }

    va_lock(p->ctx);

    if (p->buffer_acquired) {
        status = vaReleaseBufferHandle(p->display, p->current_image.buf);
        CHECK_VA_STATUS(p, "vaReleaseBufferHandle()");
        p->buffer_acquired = false;
    }
    if (p->current_image.image_id != VA_INVALID_ID) {
        status = vaDestroyImage(p->display, p->current_image.image_id);
        CHECK_VA_STATUS(p, "vaDestroyImage()");
        p->current_image.image_id = VA_INVALID_ID;
    }

    va_unlock(p->ctx);
}

static void destroy_textures(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;

    gl->DeleteTextures(4, p->gl_textures);
    for (int n = 0; n < 4; n++)
        p->gl_textures[n] = 0;
}

static void destroy(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    unmap_frame(hw);
    destroy_textures(hw);
    if (p->ctx)
        hwdec_devices_remove(hw->devs, &p->ctx->hwctx);
    va_destroy(p->ctx);
}

static int create(struct gl_hwdec *hw)
{
    GL *gl = hw->gl;

    struct priv *p = talloc_zero(hw, struct priv);
    hw->priv = p;
    p->current_image.buf = p->current_image.image_id = VA_INVALID_ID;
    p->log = hw->log;

    if (!eglGetCurrentContext())
        return -1;

    const char *exts = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
    if (!exts)
        return -1;

    if (!strstr(exts, "EXT_image_dma_buf_import") ||
        !strstr(exts, "EGL_KHR_image_base") ||
        !strstr(gl->extensions, "GL_OES_EGL_image") ||
        !(gl->mpgl_caps & MPGL_CAP_TEX_RG))
        return -1;

    // EGL_KHR_image_base
    p->CreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    p->DestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    // GL_OES_EGL_image
    p->EGLImageTargetTexture2DOES =
        (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!p->CreateImageKHR || !p->DestroyImageKHR ||
        !p->EGLImageTargetTexture2DOES)
        return -1;

    p->display = create_native_va_display(gl);
    if (!p->display)
        return -1;

    p->ctx = va_initialize(p->display, p->log, true);
    if (!p->ctx) {
        vaTerminate(p->display);
        return -1;
    }

    if (hw->probing && va_guess_if_emulated(p->ctx)) {
        destroy(hw);
        return -1;
    }

    MP_VERBOSE(p, "using VAAPI EGL interop\n");

    if (!test_format(hw)) {
        destroy(hw);
        return -1;
    }

    p->ctx->hwctx.driver_name = hw->driver->name;
    hwdec_devices_add(hw->devs, &p->ctx->hwctx);
    return 0;
}

static int reinit(struct gl_hwdec *hw, struct mp_image_params *params)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;

    // Recreate them to get rid of all previous image data (possibly).
    destroy_textures(hw);

    gl->GenTextures(4, p->gl_textures);
    for (int n = 0; n < 4; n++) {
        gl->BindTexture(GL_TEXTURE_2D, p->gl_textures[n]);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    gl->BindTexture(GL_TEXTURE_2D, 0);

    p->current_mpfmt = params->hw_subfmt;
    if (p->current_mpfmt != IMGFMT_NV12 &&
        p->current_mpfmt != IMGFMT_420P)
    {
        MP_FATAL(p, "unsupported VA image format %s\n",
                 mp_imgfmt_to_name(p->current_mpfmt));
        return -1;
    }

    MP_VERBOSE(p, "hw format: %s\n", mp_imgfmt_to_name(p->current_mpfmt));

    params->imgfmt = p->current_mpfmt;
    params->hw_subfmt = 0;

    return 0;
}

#define ADD_ATTRIB(name, value)                         \
    do {                                                \
    assert(num_attribs + 3 < MP_ARRAY_SIZE(attribs));   \
    attribs[num_attribs++] = (name);                    \
    attribs[num_attribs++] = (value);                   \
    attribs[num_attribs] = EGL_NONE;                    \
    } while(0)

static int map_frame(struct gl_hwdec *hw, struct mp_image *hw_image,
                     struct gl_hwdec_frame *out_frame)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;
    VAStatus status;
    VAImage *va_image = &p->current_image;

    unmap_frame(hw);

    va_lock(p->ctx);

    status = vaDeriveImage(p->display, va_surface_id(hw_image), va_image);
    if (!CHECK_VA_STATUS(p, "vaDeriveImage()"))
        goto err;

    int mpfmt = va_fourcc_to_imgfmt(va_image->format.fourcc);
    if (p->current_mpfmt != mpfmt) {
        MP_FATAL(p, "mid-stream hwdec format change (%s -> %s) not supported\n",
                 mp_imgfmt_to_name(p->current_mpfmt), mp_imgfmt_to_name(mpfmt));
        goto err;
    }

    VABufferInfo buffer_info = {.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME};
    status = vaAcquireBufferHandle(p->display, va_image->buf, &buffer_info);
    if (!CHECK_VA_STATUS(p, "vaAcquireBufferHandle()"))
        goto err;
    p->buffer_acquired = true;

    struct mp_image layout = {0};
    mp_image_set_params(&layout, &hw_image->params);
    mp_image_setfmt(&layout, mpfmt);

    // (it would be nice if we could use EGL_IMAGE_INTERNAL_FORMAT_EXT)
    int drm_fmts[4] = {MP_FOURCC('R', '8', ' ', ' '),   // DRM_FORMAT_R8
                       MP_FOURCC('G', 'R', '8', '8'),   // DRM_FORMAT_GR88
                       MP_FOURCC('R', 'G', '2', '4'),   // DRM_FORMAT_RGB888
                       MP_FOURCC('R', 'A', '2', '4')};  // DRM_FORMAT_RGBA8888

    for (int n = 0; n < layout.num_planes; n++) {
        int attribs[20] = {EGL_NONE};
        int num_attribs = 0;

        ADD_ATTRIB(EGL_LINUX_DRM_FOURCC_EXT, drm_fmts[layout.fmt.bytes[n] - 1]);
        ADD_ATTRIB(EGL_WIDTH, mp_image_plane_w(&layout, n));
        ADD_ATTRIB(EGL_HEIGHT, mp_image_plane_h(&layout, n));
        ADD_ATTRIB(EGL_DMA_BUF_PLANE0_FD_EXT, buffer_info.handle);
        ADD_ATTRIB(EGL_DMA_BUF_PLANE0_OFFSET_EXT, va_image->offsets[n]);
        ADD_ATTRIB(EGL_DMA_BUF_PLANE0_PITCH_EXT, va_image->pitches[n]);

        p->images[n] = p->CreateImageKHR(eglGetCurrentDisplay(),
            EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
        if (!p->images[n])
            goto err;

        gl->BindTexture(GL_TEXTURE_2D, p->gl_textures[n]);
        p->EGLImageTargetTexture2DOES(GL_TEXTURE_2D, p->images[n]);

        out_frame->planes[n] = (struct gl_hwdec_plane){
            .gl_texture = p->gl_textures[n],
            .gl_target = GL_TEXTURE_2D,
            .tex_w = mp_image_plane_w(&layout, n),
            .tex_h = mp_image_plane_h(&layout, n),
        };
    }
    gl->BindTexture(GL_TEXTURE_2D, 0);

    if (va_image->format.fourcc == VA_FOURCC_YV12)
        MPSWAP(struct gl_hwdec_plane, out_frame->planes[1], out_frame->planes[2]);

    va_unlock(p->ctx);
    return 0;

err:
    va_unlock(p->ctx);
    MP_FATAL(p, "mapping VAAPI EGL image failed\n");
    unmap_frame(hw);
    return -1;
}

static bool test_format(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    bool ok = false;

    struct mp_image_pool *alloc = mp_image_pool_new(1);
    va_pool_set_allocator(alloc, p->ctx, VA_RT_FORMAT_YUV420);
    struct mp_image *surface = mp_image_pool_get(alloc, IMGFMT_VAAPI, 64, 64);
    if (surface) {
        va_surface_init_subformat(surface);
        struct mp_image_params params = surface->params;
        if (reinit(hw, &params) >= 0) {
            struct gl_hwdec_frame frame = {0};
            ok = map_frame(hw, surface, &frame) >= 0;
        }
        unmap_frame(hw);
    }
    talloc_free(surface);
    talloc_free(alloc);

    return ok;
}

const struct gl_hwdec_driver gl_hwdec_vaegl = {
    .name = "vaapi-egl",
    .api = HWDEC_VAAPI,
    .imgfmt = IMGFMT_VAAPI,
    .create = create,
    .reinit = reinit,
    .map_frame = map_frame,
    .unmap = unmap_frame,
    .destroy = destroy,
};
