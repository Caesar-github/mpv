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
 *
 * You can alternatively redistribute this file and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stddef.h>
#include <assert.h>

#include "common/common.h"
#include "video/out/x11_common.h"
#include "common.h"

#include "rpi.h"

static void *get_proc_address(const GLubyte *name)
{
    void *p = eglGetProcAddress(name);
    // EGL 1.4 (supported by the RPI firmware) does not necessarily return
    // function pointers for core functions.
    if (!p) {
        void *h = dlopen("/opt/vc/lib/libGLESv2.so", RTLD_LAZY);
        if (h) {
            p = dlsym(h, name);
            dlclose(h);
        }
    }
    return p;
}

static EGLConfig select_fb_config_egl(struct mp_egl_rpi *p)
{
    EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint config_count;
    EGLConfig config;

    eglChooseConfig(p->egl_display, attributes, &config, 1, &config_count);

    if (!config_count) {
        MP_FATAL(p, "Could find EGL configuration!\n");
        return NULL;
    }

    return config;
}

int mp_egl_rpi_init(struct mp_egl_rpi *p, DISPMANX_ELEMENT_HANDLE_T window,
                    int w, int h)
{
    p->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(p->egl_display, NULL, NULL)) {
        MP_FATAL(p, "EGL failed to initialize.\n");
        goto fail;
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    EGLConfig config = select_fb_config_egl(p);
    if (!config)
        goto fail;

    p->egl_window = (EGL_DISPMANX_WINDOW_T){
        .element = window,
        .width = w,
        .height = h,
    };
    p->egl_surface = eglCreateWindowSurface(p->egl_display, config,
                                            &p->egl_window, NULL);

    if (p->egl_surface == EGL_NO_SURFACE) {
        MP_FATAL(p, "Could not create EGL surface!\n");
        goto fail;
    }

    EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    p->egl_context = eglCreateContext(p->egl_display, config,
                                      EGL_NO_CONTEXT, context_attributes);

    if (p->egl_context == EGL_NO_CONTEXT) {
        MP_FATAL(p, "Could not create EGL context!\n");
        goto fail;
    }

    eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface,
                   p->egl_context);

    p->gl = talloc_zero(NULL, struct GL);

    const char *exts = eglQueryString(p->egl_display, EGL_EXTENSIONS);
    mpgl_load_functions(p->gl, get_proc_address, exts, p->log);

    if (!p->gl->version && !p->gl->es)
        goto fail;

    return 0;

fail:
    mp_egl_rpi_destroy(p);
    return -1;
}

void mp_egl_rpi_destroy(struct mp_egl_rpi *p)
{
    if (p->egl_display) {
        eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
    }
    if (p->egl_surface)
        eglDestroySurface(p->egl_display, p->egl_surface);
    if (p->egl_context)
        eglDestroyContext(p->egl_display, p->egl_context);
    p->egl_context = EGL_NO_CONTEXT;
    eglReleaseThread();
    p->egl_display = EGL_NO_DISPLAY;
    talloc_free(p->gl);
    p->gl = NULL;
}

struct priv {
    DISPMANX_DISPLAY_HANDLE_T display;
    DISPMANX_ELEMENT_HANDLE_T window;
    DISPMANX_UPDATE_HANDLE_T update;
    struct mp_egl_rpi egl;
    int w, h;
};

static void rpi_uninit(MPGLContext *ctx)
{
    struct priv *p = ctx->priv;
    mp_egl_rpi_destroy(&p->egl);
    if (p->display)
        vc_dispmanx_display_close(p->display);
}

static int rpi_init(struct MPGLContext *ctx, int flags)
{
    struct priv *p = ctx->priv;
    struct vo *vo = ctx->vo;

    p->egl.log = vo->log;

    bcm_host_init();

    p->display = vc_dispmanx_display_open(0);
    p->update = vc_dispmanx_update_start(0);
    if (!p->display || !p->update) {
        MP_FATAL(ctx->vo, "Could not get DISPMANX objects.\n");
        goto fail;
    }

    uint32_t w, h;
    if (graphics_get_display_size(0, &w, &h) < 0) {
        MP_FATAL(ctx->vo, "Could not get display size.\n");
        goto fail;
    }

    // dispmanx is like a neanderthal version of Wayland - you can add an
    // overlay any place on the screen. Just use the whole screen.
    VC_RECT_T dst = {.width = w, .height = h};
    VC_RECT_T src = {.width = w << 16, .height = h << 16};
    VC_DISPMANX_ALPHA_T alpha = {
        .flags = DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS,
        .opacity = 0xFF,
    };
    p->window = vc_dispmanx_element_add(p->update, p->display, 1, &dst, 0,
                                        &src, DISPMANX_PROTECTION_NONE, &alpha, 0, 0);
    if (!p->window) {
        MP_FATAL(ctx->vo, "Could not add DISPMANX element.\n");
        goto fail;
    }

    vc_dispmanx_update_submit_sync(p->update);

    if (mp_egl_rpi_init(&p->egl, p->window, w, h) < 0)
        goto fail;

    ctx->gl = p->egl.gl;

    vo->dwidth = p->w = w;
    vo->dheight = p->h = h;

    return 0;

fail:
    rpi_uninit(ctx);
    return -1;
}

static int rpi_reconfig(struct MPGLContext *ctx)
{
    struct priv *p = ctx->priv;
    ctx->vo->dwidth = p->w;
    ctx->vo->dheight = p->h;
    return 0;
}

static void rpi_swap_buffers(MPGLContext *ctx)
{
    struct priv *p = ctx->priv;
    eglSwapBuffers(p->egl.egl_display, p->egl.egl_surface);
}

static int rpi_control(MPGLContext *ctx, int *events, int request, void *arg)
{
    return VO_NOTIMPL;
}

const struct mpgl_driver mpgl_driver_rpi = {
    .name           = "rpi",
    .priv_size      = sizeof(struct priv),
    .init           = rpi_init,
    .reconfig       = rpi_reconfig,
    .swap_buffers   = rpi_swap_buffers,
    .control        = rpi_control,
    .uninit         = rpi_uninit,
};