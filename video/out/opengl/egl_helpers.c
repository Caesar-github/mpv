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

#include "config.h"

#if HAVE_LIBDL
#include <dlfcn.h>
#endif

#include "common/common.h"

#include "egl_helpers.h"
#include "common.h"
#include "context.h"

#if HAVE_EGL_ANGLE
// On Windows, egl_helpers.c is only used by ANGLE, where the EGL functions may
// be loaded dynamically from ANGLE DLLs
#include "angle_dynamic.h"
#endif

// EGL 1.5
#ifndef EGL_CONTEXT_OPENGL_PROFILE_MASK
#define EGL_CONTEXT_MAJOR_VERSION               0x3098
#define EGL_CONTEXT_MINOR_VERSION               0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK         0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT     0x00000001
#define EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE   0x31B1
#define EGL_OPENGL_ES3_BIT                      0x00000040
#endif

// es_version = 0 (desktop), 2/3 (ES major version)
static bool create_context(EGLDisplay display, struct mp_log *log, bool probing,
                           int es_version, struct mpegl_opts *opts,
                           EGLContext *out_context, EGLConfig *out_config)
{
    int msgl = probing ? MSGL_V : MSGL_FATAL;

    EGLenum api = EGL_OPENGL_API;
    EGLint rend = EGL_OPENGL_BIT;
    const char *name = "Desktop OpenGL";
    if (es_version == 2) {
        api = EGL_OPENGL_ES_API;
        rend = EGL_OPENGL_ES2_BIT;
        name = "GLES 2.0";
    }
    if (es_version == 3) {
        api = EGL_OPENGL_ES_API;
        rend = EGL_OPENGL_ES3_BIT;
        name = "GLES 3.x";
    }

    mp_msg(log, MSGL_V, "Trying to create %s context.\n", name);

    if (!eglBindAPI(api)) {
        mp_msg(log, MSGL_V, "Could not bind API!\n");
        return false;
    }


    EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_ALPHA_SIZE, (opts->vo_flags & VOFLAG_ALPHA ) ? 1 : 0,
        EGL_RENDERABLE_TYPE, rend,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(display, attributes, NULL, 0, &num_configs))
        num_configs = 0;

    EGLConfig *configs = talloc_array(NULL, EGLConfig, num_configs);
    if (!eglChooseConfig(display, attributes, configs, num_configs, &num_configs))
        num_configs = 0;

    if (!num_configs) {
        talloc_free(configs);
        mp_msg(log, msgl, "Could not choose EGLConfig!\n");
        return false;
    }

    int chosen = 0;
    if (opts->refine_config)
        chosen = opts->refine_config(opts->user_data, configs, num_configs);
    EGLConfig config = configs[chosen];

    talloc_free(configs);

    EGLContext *ctx = NULL;

    if (es_version) {
        EGLint attrs[] = {
            EGL_CONTEXT_CLIENT_VERSION, es_version,
            EGL_NONE
        };

        ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, attrs);
    } else {
        for (int n = 0; mpgl_preferred_gl_versions[n]; n++) {
            int ver = mpgl_preferred_gl_versions[n];

            EGLint attrs[] = {
                EGL_CONTEXT_MAJOR_VERSION, MPGL_VER_GET_MAJOR(ver),
                EGL_CONTEXT_MINOR_VERSION, MPGL_VER_GET_MINOR(ver),
                EGL_CONTEXT_OPENGL_PROFILE_MASK,
                    ver >= 320 ? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT : 0,
                EGL_NONE
            };

            ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, attrs);
            if (ctx)
                break;
        }

        if (!ctx) {
            // Fallback for EGL 1.4 without EGL_KHR_create_context.
            EGLint attrs[] = { EGL_NONE };

            ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, attrs);
        }
    }

    if (!ctx) {
        mp_msg(log, msgl, "Could not create EGL context!\n");
        return false;
    }

    *out_context = ctx;
    *out_config = config;
    return true;
}

#define STR_OR_ERR(s) ((s) ? (s) : "(error)")

// Create a context and return it and the config it was created with. If it
// returns false, the out_* pointers are set to NULL.
// vo_flags is a combination of VOFLAG_* values.
bool mpegl_create_context(EGLDisplay display, struct mp_log *log, int vo_flags,
                          EGLContext *out_context, EGLConfig *out_config)
{
    return mpegl_create_context_opts(display, log,
        &(struct mpegl_opts){.vo_flags = vo_flags}, out_context, out_config);
}

// Create a context and return it and the config it was created with. If it
// returns false, the out_* pointers are set to NULL.
bool mpegl_create_context_opts(EGLDisplay display, struct mp_log *log,
                               struct mpegl_opts *opts,
                               EGLContext *out_context, EGLConfig *out_config)
{
    assert(opts);

    *out_context = NULL;
    *out_config = NULL;

    const char *version = eglQueryString(display, EGL_VERSION);
    const char *vendor = eglQueryString(display, EGL_VENDOR);
    const char *apis = eglQueryString(display, EGL_CLIENT_APIS);
    mp_verbose(log, "EGL_VERSION=%s\nEGL_VENDOR=%s\nEGL_CLIENT_APIS=%s\n",
               STR_OR_ERR(version), STR_OR_ERR(vendor), STR_OR_ERR(apis));

    bool probing = opts->vo_flags & VOFLAG_PROBING;
    int msgl = probing ? MSGL_V : MSGL_FATAL;
    bool try_gles = !(opts->vo_flags & VOFLAG_NO_GLES);

    if (!(opts->vo_flags & VOFLAG_GLES)) {
        // Desktop OpenGL
        if (create_context(display, log, try_gles | probing, 0, opts,
                           out_context, out_config))
            return true;
    }

    if (try_gles && !(opts->vo_flags & VOFLAG_GLES2)) {
        // ES 3.x
        if (create_context(display, log, true, 3, opts,
                           out_context, out_config))
            return true;
    }

    if (try_gles) {
        // ES 2.0
        if (create_context(display, log, probing, 2, opts,
                           out_context, out_config))
            return true;
    }

    mp_msg(log, msgl, "Could not create a GL context.\n");
    return false;
}

static void *mpegl_get_proc_address(void *ctx, const char *name)
{
    void *p = eglGetProcAddress(name);
#if defined(__GLIBC__) && HAVE_LIBDL
    // Some crappy ARM/Linux things do not provide EGL 1.5, so above call does
    // not necessarily return function pointers for core functions. Try to get
    // them from a loaded GLES lib. As POSIX leaves RTLD_DEFAULT "reserved",
    // use it only with glibc.
    if (!p)
        p = dlsym(RTLD_DEFAULT, name);
#endif
    return p;
}

// Load gl version and function pointers into *gl.
// Expects a current EGL context set.
void mpegl_load_functions(struct GL *gl, struct mp_log *log)
{
    const char *egl_exts = "";
    EGLDisplay display = eglGetCurrentDisplay();
    if (display != EGL_NO_DISPLAY)
        egl_exts = eglQueryString(display, EGL_EXTENSIONS);

    mpgl_load_functions2(gl, mpegl_get_proc_address, NULL, egl_exts, log);
}
