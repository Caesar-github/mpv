/*
 * common OpenGL routines
 *
 * copyleft (C) 2005-2010 Reimar Döffinger <Reimar.Doeffinger@gmx.de>
 * Special thanks go to the xine team and Matthias Hopf, whose video_out_opengl.c
 * gave me lots of good ideas.
 *
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>

#include "context.h"
#include "common/common.h"
#include "options/options.h"
#include "options/m_option.h"

extern const struct mpgl_driver mpgl_driver_x11;
extern const struct mpgl_driver mpgl_driver_x11egl;
extern const struct mpgl_driver mpgl_driver_x11_probe;
extern const struct mpgl_driver mpgl_driver_drm_egl;
extern const struct mpgl_driver mpgl_driver_cocoa;
extern const struct mpgl_driver mpgl_driver_wayland;
extern const struct mpgl_driver mpgl_driver_w32;
extern const struct mpgl_driver mpgl_driver_angle;
extern const struct mpgl_driver mpgl_driver_dxinterop;
extern const struct mpgl_driver mpgl_driver_rpi;

static const struct mpgl_driver *const backends[] = {
#if HAVE_RPI
    &mpgl_driver_rpi,
#endif
#if HAVE_GL_COCOA
    &mpgl_driver_cocoa,
#endif
#if HAVE_EGL_ANGLE
    &mpgl_driver_angle,
#endif
#if HAVE_GL_WIN32
    &mpgl_driver_w32,
#endif
#if HAVE_GL_DXINTEROP
    &mpgl_driver_dxinterop,
#endif
#if HAVE_GL_WAYLAND
    &mpgl_driver_wayland,
#endif
#if HAVE_GL_X11
    &mpgl_driver_x11_probe,
#endif
#if HAVE_EGL_X11
    &mpgl_driver_x11egl,
#endif
#if HAVE_GL_X11
    &mpgl_driver_x11,
#endif
#if HAVE_EGL_DRM
    &mpgl_driver_drm_egl,
#endif
};

int mpgl_find_backend(const char *name)
{
    if (name == NULL || strcmp(name, "auto") == 0)
        return -1;
    for (int n = 0; n < MP_ARRAY_SIZE(backends); n++) {
        if (strcmp(backends[n]->name, name) == 0)
            return n;
    }
    return -2;
}

int mpgl_validate_backend_opt(struct mp_log *log, const struct m_option *opt,
                              struct bstr name, struct bstr param)
{
    if (bstr_equals0(param, "help")) {
        mp_info(log, "OpenGL windowing backends:\n");
        mp_info(log, "    auto (autodetect)\n");
        for (int n = 0; n < MP_ARRAY_SIZE(backends); n++)
            mp_info(log, "    %s\n", backends[n]->name);
        return M_OPT_EXIT - 1;
    }
    char s[20];
    snprintf(s, sizeof(s), "%.*s", BSTR_P(param));
    return mpgl_find_backend(s) >= -1 ? 1 : M_OPT_INVALID;
}

#if HAVE_C11_TLS
#define MP_TLS _Thread_local
#elif defined(__GNU__)
#define MP_TLS __thread
#endif

#ifdef MP_TLS
static MP_TLS MPGLContext *current_context;

static void * GLAPIENTRY get_native_display(const char *name)
{
    if (current_context && current_context->native_display_type &&
        name && strcmp(current_context->native_display_type, name) == 0)
        return current_context->native_display;
    return NULL;
}

static void set_current_context(MPGLContext *context)
{
    current_context = context;
    if (context && !context->gl->MPGetNativeDisplay)
        context->gl->MPGetNativeDisplay = get_native_display;
}
#else
static void set_current_context(MPGLContext *context)
{
}
#endif

static MPGLContext *init_backend(struct vo *vo, const struct mpgl_driver *driver,
                                 bool probing, int vo_flags)
{
    MPGLContext *ctx = talloc_ptrtype(NULL, ctx);
    *ctx = (MPGLContext) {
        .gl = talloc_zero(ctx, GL),
        .vo = vo,
        .driver = driver,
    };
    bool old_probing = vo->probing;
    vo->probing = probing; // hack; kill it once backends are separate
    MP_VERBOSE(vo, "Initializing OpenGL backend '%s'\n", ctx->driver->name);
    ctx->priv = talloc_zero_size(ctx, ctx->driver->priv_size);
    if (ctx->driver->init(ctx, vo_flags) < 0) {
        vo->probing = old_probing;
        talloc_free(ctx);
        return NULL;
    }
    vo->probing = old_probing;

    if (!ctx->gl->version && !ctx->gl->es)
        goto cleanup;

    if (probing && ctx->gl->es && (vo_flags & VOFLAG_NO_GLES)) {
        MP_VERBOSE(ctx->vo, "Skipping GLES backend.\n");
        goto cleanup;
    }

    if (ctx->gl->mpgl_caps & MPGL_CAP_SW) {
        MP_WARN(ctx->vo, "Suspected software renderer or indirect context.\n");
        if (vo->probing && !(vo_flags & VOFLAG_SW))
            goto cleanup;
    }

    ctx->gl->debug_context = !!(vo_flags & VOFLAG_GL_DEBUG);

    set_current_context(ctx);

    return ctx;

cleanup:
    mpgl_uninit(ctx);
    return NULL;
}

// Create a VO window and create a GL context on it.
//  vo_flags: passed to the backend's create window function
MPGLContext *mpgl_init(struct vo *vo, const char *backend_name, int vo_flags)
{
    MPGLContext *ctx = NULL;
    int index = mpgl_find_backend(backend_name);
    if (index == -1) {
        for (int n = 0; n < MP_ARRAY_SIZE(backends); n++) {
            ctx = init_backend(vo, backends[n], true, vo_flags);
            if (ctx)
                break;
        }
        // VO forced, but no backend is ok => force the first that works at all
        if (!ctx && !vo->probing) {
            for (int n = 0; n < MP_ARRAY_SIZE(backends); n++) {
                ctx = init_backend(vo, backends[n], false, vo_flags);
                if (ctx)
                    break;
            }
        }
    } else if (index >= 0) {
        ctx = init_backend(vo, backends[index], false, vo_flags);
    }
    return ctx;
}

int mpgl_reconfig_window(struct MPGLContext *ctx)
{
    return ctx->driver->reconfig(ctx);
}

int mpgl_control(struct MPGLContext *ctx, int *events, int request, void *arg)
{
    return ctx->driver->control(ctx, events, request, arg);
}

void mpgl_swap_buffers(struct MPGLContext *ctx)
{
    ctx->driver->swap_buffers(ctx);
}

void mpgl_uninit(MPGLContext *ctx)
{
    set_current_context(NULL);
    if (ctx)
        ctx->driver->uninit(ctx);
    talloc_free(ctx);
}
