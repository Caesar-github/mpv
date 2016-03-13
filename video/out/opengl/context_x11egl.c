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

#include <assert.h>

#include <X11/Xlib.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "common/common.h"
#include "video/out/x11_common.h"
#include "context.h"
#include "egl_helpers.h"

struct priv {
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
};

static void mpegl_uninit(MPGLContext *ctx)
{
    struct priv *p = ctx->priv;
    if (p->egl_context) {
        eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroyContext(p->egl_display, p->egl_context);
    }
    p->egl_context = EGL_NO_CONTEXT;
    vo_x11_uninit(ctx->vo);
}

static EGLConfig select_fb_config_egl(struct MPGLContext *ctx, bool es)
{
    struct priv *p = ctx->priv;

    EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_RENDERABLE_TYPE, es ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_BIT,
        EGL_NONE
    };

    EGLint config_count;
    EGLConfig config;

    eglChooseConfig(p->egl_display, attributes, &config, 1, &config_count);

    if (!config_count) {
        MP_FATAL(ctx->vo, "Could not find EGL configuration!\n");
        return NULL;
    }

    return config;
}

static bool create_context_egl(MPGLContext *ctx, EGLConfig config,
                               EGLNativeWindowType window, bool es)
{
    struct priv *p = ctx->priv;

    EGLint context_attributes[] = {
        // aka EGL_CONTEXT_MAJOR_VERSION_KHR
        EGL_CONTEXT_CLIENT_VERSION, es ? 2 : 3,
        EGL_NONE
    };

    p->egl_surface = eglCreateWindowSurface(p->egl_display, config, window, NULL);

    if (p->egl_surface == EGL_NO_SURFACE) {
        MP_FATAL(ctx->vo, "Could not create EGL surface!\n");
        return false;
    }

    p->egl_context = eglCreateContext(p->egl_display, config,
                                      EGL_NO_CONTEXT, context_attributes);

    if (p->egl_context == EGL_NO_CONTEXT) {
        MP_FATAL(ctx->vo, "Could not create EGL context!\n");
        return false;
    }

    eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface,
                   p->egl_context);

    return true;
}

static int mpegl_init(struct MPGLContext *ctx, int flags)
{
    struct priv *p = ctx->priv;
    struct vo *vo = ctx->vo;
    bool es = flags & VOFLAG_GLES;
    int msgl = vo->probing ? MSGL_V : MSGL_FATAL;

    if (!vo_x11_init(vo))
        goto uninit;

    if (!eglBindAPI(es ? EGL_OPENGL_ES_API : EGL_OPENGL_API)) {
        mp_msg(vo->log, msgl, "Could not bind API (%s).\n", es ? "GLES" : "GL");
        goto uninit;
    }

    p->egl_display = eglGetDisplay(vo->x11->display);
    if (!eglInitialize(p->egl_display, NULL, NULL)) {
        mp_msg(vo->log, msgl, "Could not initialize EGL.\n");
        goto uninit;
    }

    EGLConfig config = select_fb_config_egl(ctx, es);
    if (!config)
        goto uninit;

    int vID, n;
    eglGetConfigAttrib(p->egl_display, config, EGL_NATIVE_VISUAL_ID, &vID);
    XVisualInfo template = {.visualid = vID};
    XVisualInfo *vi = XGetVisualInfo(vo->x11->display, VisualIDMask, &template, &n);

    if (!vi) {
        MP_FATAL(vo, "Getting X visual failed!\n");
        goto uninit;
    }

    if (!vo_x11_create_vo_window(vo, vi, "gl")) {
        XFree(vi);
        goto uninit;
    }

    XFree(vi);

    if (!create_context_egl(ctx, config, (EGLNativeWindowType)vo->x11->window, es))
        goto uninit;

    const char *egl_exts = eglQueryString(p->egl_display, EGL_EXTENSIONS);

    void *(*gpa)(const GLubyte*) = (void *(*)(const GLubyte*))eglGetProcAddress;
    mpgl_load_functions(ctx->gl, gpa, egl_exts, vo->log);
    mp_egl_get_depth(ctx->gl, config);

    ctx->native_display_type = "x11";
    ctx->native_display = vo->x11->display;
    return 0;

uninit:
    mpegl_uninit(ctx);
    return -1;
}

static int mpegl_reconfig(struct MPGLContext *ctx)
{
    vo_x11_config_vo_window(ctx->vo);
    return 0;
}

static int mpegl_control(struct MPGLContext *ctx, int *events, int request,
                         void *arg)
{
    return vo_x11_control(ctx->vo, events, request, arg);
}

static void mpegl_swap_buffers(MPGLContext *ctx)
{
    struct priv *p = ctx->priv;
    eglSwapBuffers(p->egl_display, p->egl_surface);
}

const struct mpgl_driver mpgl_driver_x11egl = {
    .name           = "x11egl",
    .priv_size      = sizeof(struct priv),
    .init           = mpegl_init,
    .reconfig       = mpegl_reconfig,
    .swap_buffers   = mpegl_swap_buffers,
    .control        = mpegl_control,
    .uninit         = mpegl_uninit,
};
