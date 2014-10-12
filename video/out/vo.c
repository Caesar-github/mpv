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
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>

#ifndef __MINGW32__
#include <unistd.h>
#include <poll.h>
#endif

#include "talloc.h"

#include "config.h"
#include "osdep/timer.h"
#include "osdep/threads.h"
#include "misc/dispatch.h"
#include "misc/rendezvous.h"
#include "options/options.h"
#include "misc/bstr.h"
#include "vo.h"
#include "aspect.h"
#include "input/input.h"
#include "options/m_config.h"
#include "common/msg.h"
#include "common/global.h"
#include "video/mp_image.h"
#include "video/vfcap.h"
#include "sub/osd.h"
#include "osdep/io.h"

extern const struct vo_driver video_out_x11;
extern const struct vo_driver video_out_vdpau;
extern const struct vo_driver video_out_xv;
extern const struct vo_driver video_out_opengl;
extern const struct vo_driver video_out_opengl_hq;
extern const struct vo_driver video_out_opengl_old;
extern const struct vo_driver video_out_null;
extern const struct vo_driver video_out_image;
extern const struct vo_driver video_out_lavc;
extern const struct vo_driver video_out_caca;
extern const struct vo_driver video_out_direct3d;
extern const struct vo_driver video_out_direct3d_shaders;
extern const struct vo_driver video_out_sdl;
extern const struct vo_driver video_out_vaapi;
extern const struct vo_driver video_out_wayland;

const struct vo_driver *const video_out_drivers[] =
{
#if HAVE_GL
        &video_out_opengl,
#endif
#if HAVE_VDPAU
        &video_out_vdpau,
#endif
#if HAVE_DIRECT3D
        &video_out_direct3d_shaders,
        &video_out_direct3d,
#endif
#if HAVE_XV
        &video_out_xv,
#endif
#if HAVE_SDL2
        &video_out_sdl,
#endif
#if HAVE_GL
        &video_out_opengl_old,
#endif
#if HAVE_VAAPI
        &video_out_vaapi,
#endif
#if HAVE_X11
        &video_out_x11,
#endif
        &video_out_null,
        // should not be auto-selected
        &video_out_image,
#if HAVE_CACA
        &video_out_caca,
#endif
#if HAVE_ENCODING
        &video_out_lavc,
#endif
#if HAVE_GL
        &video_out_opengl_hq,
#endif
#if HAVE_WAYLAND
        &video_out_wayland,
#endif
        NULL
};

struct vo_internal {
    pthread_t thread;
    struct mp_dispatch_queue *dispatch;

    // --- The following fields are protected by lock
    pthread_mutex_t lock;
    pthread_cond_t wakeup;

    bool need_wakeup;
    bool terminate;

    int wakeup_pipe[2]; // used for VOs that use a unix FD for waiting


    bool hasframe;
    bool hasframe_rendered;
    bool request_redraw;            // redraw request from player to VO
    bool want_redraw;               // redraw request from VO to player
    bool paused;

    int64_t flip_queue_offset; // queue flip events at most this much in advance

    int64_t drop_count;
    bool dropped_frame;             // the previous frame was dropped
    struct mp_image *dropped_image; // used to possibly redraw the dropped frame

    int64_t wakeup_pts;             // time at which to pull frame from decoder

    bool rendering;                 // true if an image is being rendered
    struct mp_image *frame_queued;  // the image that should be rendered
    int64_t frame_pts;              // realtime of intended display
    int64_t frame_duration;         // realtime frame duration (for framedrop)

    // --- The following fields can be accessed from the VO thread only
    int64_t vsync_interval;
    int64_t last_flip;
    char *window_title;
};

static void forget_frames(struct vo *vo);
static void *vo_thread(void *ptr);

static bool get_desc(struct m_obj_desc *dst, int index)
{
    if (index >= MP_ARRAY_SIZE(video_out_drivers) - 1)
        return false;
    const struct vo_driver *vo = video_out_drivers[index];
    *dst = (struct m_obj_desc) {
        .name = vo->name,
        .description = vo->description,
        .priv_size = vo->priv_size,
        .priv_defaults = vo->priv_defaults,
        .options = vo->options,
        .hidden = vo->encode,
        .p = vo,
    };
    return true;
}

// For the vo option
const struct m_obj_list vo_obj_list = {
    .get_desc = get_desc,
    .description = "video outputs",
    .aliases = {
        {"gl",        "opengl"},
        {"gl3",       "opengl-hq"},
        {0}
    },
    .allow_unknown_entries = true,
    .allow_trailer = true,
};

static void dispatch_wakeup_cb(void *ptr)
{
    struct vo *vo = ptr;
    vo_wakeup(vo);
}

// Does not include thread- and VO uninit.
static void dealloc_vo(struct vo *vo)
{
    forget_frames(vo); // implicitly synchronized
    pthread_mutex_destroy(&vo->in->lock);
    pthread_cond_destroy(&vo->in->wakeup);
    for (int n = 0; n < 2; n++)
        close(vo->in->wakeup_pipe[n]);
    talloc_free(vo);
}

static struct vo *vo_create(struct mpv_global *global,
                            struct input_ctx *input_ctx, struct osd_state *osd,
                            struct encode_lavc_context *encode_lavc_ctx,
                            char *name, char **args)
{
    struct mp_log *log = mp_log_new(NULL, global->log, "vo");
    struct m_obj_desc desc;
    if (!m_obj_list_find(&desc, &vo_obj_list, bstr0(name))) {
        mp_msg(log, MSGL_ERR, "Video output %s not found!\n", name);
        talloc_free(log);
        return NULL;
    };
    struct vo *vo = talloc_ptrtype(NULL, vo);
    *vo = (struct vo) {
        .log = mp_log_new(vo, log, name),
        .driver = desc.p,
        .opts = &global->opts->vo,
        .global = global,
        .encode_lavc_ctx = encode_lavc_ctx,
        .input_ctx = input_ctx,
        .osd = osd,
        .event_fd = -1,
        .monitor_par = 1,
        .in = talloc(vo, struct vo_internal),
    };
    talloc_steal(vo, log);
    *vo->in = (struct vo_internal) {
        .dispatch = mp_dispatch_create(vo),
    };
    mp_make_wakeup_pipe(vo->in->wakeup_pipe);
    mp_dispatch_set_wakeup_fn(vo->in->dispatch, dispatch_wakeup_cb, vo);
    pthread_mutex_init(&vo->in->lock, NULL);
    pthread_cond_init(&vo->in->wakeup, NULL);

    mp_input_set_mouse_transform(vo->input_ctx, NULL, NULL);
    if (vo->driver->encode != !!vo->encode_lavc_ctx)
        goto error;
    struct m_config *config = m_config_from_obj_desc(vo, vo->log, &desc);
    if (m_config_apply_defaults(config, name, vo->opts->vo_defs) < 0)
        goto error;
    if (m_config_set_obj_params(config, args) < 0)
        goto error;
    vo->priv = config->optstruct;

    if (pthread_create(&vo->in->thread, NULL, vo_thread, vo))
        goto error;
    if (mp_rendezvous(vo, 0) < 0) { // init barrier
        pthread_join(vo->in->thread, NULL);
        goto error;
    }
    return vo;

error:
    dealloc_vo(vo);
    return NULL;
}

struct vo *init_best_video_out(struct mpv_global *global,
                               struct input_ctx *input_ctx,
                               struct osd_state *osd,
                               struct encode_lavc_context *encode_lavc_ctx)
{
    struct m_obj_settings *vo_list = global->opts->vo.video_driver_list;
    // first try the preferred drivers, with their optional subdevice param:
    if (vo_list && vo_list[0].name) {
        for (int n = 0; vo_list[n].name; n++) {
            // Something like "-vo name," allows fallback to autoprobing.
            if (strlen(vo_list[n].name) == 0)
                goto autoprobe;
            struct vo *vo = vo_create(global, input_ctx, osd, encode_lavc_ctx,
                                      vo_list[n].name, vo_list[n].attribs);
            if (vo)
                return vo;
        }
        return NULL;
    }
autoprobe:
    // now try the rest...
    for (int i = 0; video_out_drivers[i]; i++) {
        struct vo *vo = vo_create(global, input_ctx, osd, encode_lavc_ctx,
                                  (char *)video_out_drivers[i]->name, NULL);
        if (vo)
            return vo;
    }
    return NULL;
}

void vo_destroy(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    mp_dispatch_lock(in->dispatch);
    vo->in->terminate = true;
    mp_dispatch_unlock(in->dispatch);
    pthread_join(vo->in->thread, NULL);
    dealloc_vo(vo);
}

// to be called from VO thread only
static void update_display_fps(struct vo *vo)
{
    double display_fps = 1000.0; // assume infinite if unset
    if (vo->global->opts->frame_drop_fps > 0) {
        display_fps = vo->global->opts->frame_drop_fps;
    } else {
        vo->driver->control(vo, VOCTRL_GET_DISPLAY_FPS, &display_fps);
    }
    int64_t n_interval = MPMAX((int64_t)(1e6 / display_fps), 1);
    if (vo->in->vsync_interval != n_interval)
        MP_VERBOSE(vo, "Assuming %f FPS for framedrop.\n", display_fps);
    vo->in->vsync_interval = n_interval;
}

static void check_vo_caps(struct vo *vo)
{
    int rot = vo->params->rotate;
    if (rot) {
        bool ok = rot % 90 ? false : (vo->driver->caps & VO_CAP_ROTATE90);
        if (!ok) {
           MP_WARN(vo, "Video is flagged as rotated by %d degrees, but the "
                   "video output does not support this.\n", rot);
        }
    }
}

static void run_reconfig(void *p)
{
    void **pp = p;
    struct vo *vo = pp[0];
    struct mp_image_params *params = pp[1];
    int flags = *(int *)pp[2];
    int *ret = pp[3];

    vo->dwidth = params->d_w;
    vo->dheight = params->d_h;

    talloc_free(vo->params);
    vo->params = talloc_memdup(vo, params, sizeof(*params));

    *ret = vo->driver->reconfig(vo, vo->params, flags);
    vo->config_ok = *ret >= 0;
    if (vo->config_ok) {
        check_vo_caps(vo);
    } else {
        talloc_free(vo->params);
        vo->params = NULL;
    }
    forget_frames(vo); // implicitly synchronized

    update_display_fps(vo);
}

int vo_reconfig(struct vo *vo, struct mp_image_params *params, int flags)
{
    int ret;
    void *p[] = {vo, params, &flags, &ret};
    mp_dispatch_run(vo->in->dispatch, run_reconfig, p);
    return ret;
}

static void run_control(void *p)
{
    void **pp = p;
    struct vo *vo = pp[0];
    uint32_t request = *(int *)pp[1];
    void *data = pp[2];
    if (request == VOCTRL_UPDATE_WINDOW_TITLE) // legacy fallback
        vo->in->window_title = talloc_strdup(vo, data);
    int ret = vo->driver->control(vo, request, data);
    *(int *)pp[3] = ret;
}

int vo_control(struct vo *vo, uint32_t request, void *data)
{
    int ret;
    void *p[] = {vo, &request, data, &ret};
    mp_dispatch_run(vo->in->dispatch, run_control, p);
    return ret;
}

// must be called locked
static void forget_frames(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    in->hasframe = false;
    in->hasframe_rendered = false;
    in->drop_count = 0;
    mp_image_unrefp(&in->frame_queued);
    mp_image_unrefp(&in->dropped_image);
}

#ifndef __MINGW32__
static void wait_event_fd(struct vo *vo, int64_t until_time)
{
    struct vo_internal *in = vo->in;

    struct pollfd fds[2] = {
        { .fd = vo->event_fd, .events = POLLIN },
        { .fd = in->wakeup_pipe[0], .events = POLLIN },
    };
    int64_t wait_us = until_time - mp_time_us();
    int timeout_ms = MPCLAMP((wait_us + 500) / 1000, 0, 10000);

    poll(fds, 2, timeout_ms);

    if (fds[1].revents & POLLIN) {
        char buf[100];
        read(in->wakeup_pipe[0], buf, sizeof(buf)); // flush
    }
}
static void wakeup_event_fd(struct vo *vo)
{
    struct vo_internal *in = vo->in;

    write(in->wakeup_pipe[1], &(char){0}, 1);
}
#else
static void wait_event_fd(struct vo *vo, int64_t until_time){}
static void wakeup_event_fd(struct vo *vo){}
#endif

// Called unlocked.
static void wait_vo(struct vo *vo, int64_t until_time)
{
    struct vo_internal *in = vo->in;

    if (vo->event_fd >= 0) {
        wait_event_fd(vo, until_time);
        pthread_mutex_lock(&in->lock);
        in->need_wakeup = false;
        pthread_mutex_unlock(&in->lock);
    } else if (vo->driver->wait_events) {
        vo->driver->wait_events(vo, until_time);
        pthread_mutex_lock(&in->lock);
        in->need_wakeup = false;
        pthread_mutex_unlock(&in->lock);
    } else {
        pthread_mutex_lock(&in->lock);
        if (!in->need_wakeup)
            mpthread_cond_timedwait(&in->wakeup, &in->lock, until_time);
        in->need_wakeup = false;
        pthread_mutex_unlock(&in->lock);
    }
}

static void wakeup_locked(struct vo *vo)
{
    struct vo_internal *in = vo->in;

    pthread_cond_signal(&in->wakeup);
    if (vo->event_fd >= 0)
        wakeup_event_fd(vo);
    if (vo->driver->wakeup)
        vo->driver->wakeup(vo);
    in->need_wakeup = true;
}

// Wakeup VO thread, and make it check for new events with VOCTRL_CHECK_EVENTS.
// To be used by threaded VO backends.
void vo_wakeup(struct vo *vo)
{
    struct vo_internal *in = vo->in;

    pthread_mutex_lock(&in->lock);
    wakeup_locked(vo);
    pthread_mutex_unlock(&in->lock);
}

// Whether vo_queue_frame() can be called. If the VO is not ready yet, the
// function will return false, and the VO will call the wakeup callback once
// it's ready.
// next_pts is the exact time when the next frame should be displayed. If the
// VO is ready, but the time is too "early", return false, and call the wakeup
// callback once the time is right.
bool vo_is_ready_for_frame(struct vo *vo, int64_t next_pts)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    bool r = vo->config_ok && !in->frame_queued;
    if (r) {
        // Don't show the frame too early - it would basically freeze the
        // display by disallowing OSD redrawing or VO interaction.
        // Actually render the frame at earliest 50ms before target time.
        next_pts -= 0.050 * 1e6;
        next_pts -= in->flip_queue_offset;
        int64_t now = mp_time_us();
        if (next_pts > now)
            r = false;
        if (!in->wakeup_pts || next_pts < in->wakeup_pts) {
            in->wakeup_pts = next_pts;
            wakeup_locked(vo);
        }
    }
    pthread_mutex_unlock(&in->lock);
    return r;
}

// Direct the VO thread to put the currently queued image on the screen.
// vo_is_ready_for_frame() must have returned true before this call.
// Ownership of the image is handed to the vo.
void vo_queue_frame(struct vo *vo, struct mp_image *image,
                    int64_t pts_us, int64_t duration)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    assert(vo->config_ok && !in->frame_queued);
    in->hasframe = true;
    in->frame_queued = image;
    in->frame_pts = pts_us;
    in->frame_duration = duration;
    in->wakeup_pts = in->frame_pts + MPMAX(duration, 0);
    wakeup_locked(vo);
    pthread_mutex_unlock(&in->lock);
}

// If a frame is currently being rendered (or queued), wait until it's done.
// Otherwise, return immediately.
void vo_wait_frame(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    while (in->frame_queued || in->rendering)
        pthread_cond_wait(&in->wakeup, &in->lock);
    pthread_mutex_unlock(&in->lock);
}

static int64_t prev_sync(struct vo *vo, int64_t ts)
{
    struct vo_internal *in = vo->in;

    int64_t diff = (int64_t)(ts - in->last_flip);
    int64_t offset = diff % in->vsync_interval;
    if (offset < 0)
        offset += in->vsync_interval;
    return ts - offset;
}

static bool render_frame(struct vo *vo)
{
    struct vo_internal *in = vo->in;

    update_display_fps(vo);

    pthread_mutex_lock(&in->lock);

    int64_t pts = in->frame_pts;
    int64_t duration = in->frame_duration;
    struct mp_image *img = in->frame_queued;
    if (!img) {
        pthread_mutex_unlock(&in->lock);
        return false;
    }

    mp_image_unrefp(&in->dropped_image);

    in->rendering = true;
    in->frame_queued = NULL;

    // The next time a flip (probably) happens.
    int64_t next_vsync = prev_sync(vo, mp_time_us()) + in->vsync_interval;
    int64_t end_time = pts + duration;

    if (!(vo->global->opts->frame_dropping & 1) || !in->hasframe_rendered ||
        vo->driver->untimed || vo->driver->encode)
        duration = -1; // disable framedrop

    in->dropped_frame = duration >= 0 && end_time < next_vsync;
    in->dropped_frame &= !(vo->driver->caps & VO_CAP_FRAMEDROP);
    // Even if we're hopelessly behind, rather degrade to 10 FPS playback,
    // instead of just freezing the display forever.
    in->dropped_frame &= mp_time_us() - in->last_flip < 100 * 1000;

    if (in->dropped_frame) {
        in->dropped_image = img;
    } else {
        in->hasframe_rendered = true;
        pthread_mutex_unlock(&in->lock);
        mp_input_wakeup(vo->input_ctx); // core can queue new video now

        MP_STATS(vo, "start video");

        vo->driver->draw_image(vo, img);

        int64_t target = pts - in->flip_queue_offset;
        while (1) {
            int64_t now = mp_time_us();
            if (target <= now)
                break;
            mp_sleep_us(target - now);
        }

        bool drop = false;
        if (vo->driver->flip_page_timed)
            drop = vo->driver->flip_page_timed(vo, pts, duration) < 1;
        else
            vo->driver->flip_page(vo);

        in->last_flip = -1;

        vo->driver->control(vo, VOCTRL_GET_RECENT_FLIP_TIME, &in->last_flip);

        if (in->last_flip < 0)
            in->last_flip = mp_time_us();

        long phase = in->last_flip % in->vsync_interval;
        MP_DBG(vo, "phase: %ld\n", phase);
        MP_STATS(vo, "value %ld phase", phase);

        MP_STATS(vo, "end video");

        pthread_mutex_lock(&in->lock);
        in->dropped_frame = drop;
    }

    if (in->dropped_frame)
        in->drop_count += 1;

    vo->want_redraw = false;

    in->want_redraw = false;
    in->request_redraw = false;
    in->rendering = false;

    pthread_cond_signal(&in->wakeup); // for vo_wait_frame()
    mp_input_wakeup(vo->input_ctx);

    pthread_mutex_unlock(&in->lock);

    return true;
}

static void do_redraw(struct vo *vo)
{
    struct vo_internal *in = vo->in;

    vo->want_redraw = false;

    pthread_mutex_lock(&in->lock);
    in->request_redraw = false;
    in->want_redraw = false;
    bool skip = !in->paused && in->dropped_frame;
    struct mp_image *img = in->dropped_image;
    if (!skip) {
        in->dropped_image = NULL;
        in->dropped_frame = false;
    }
    pthread_mutex_unlock(&in->lock);

    if (!vo->config_ok || skip)
        return;

    if (img) {
        vo->driver->draw_image(vo, img);
    } else {
        if (vo->driver->control(vo, VOCTRL_REDRAW_FRAME, NULL) < 1)
            return;
    }

    if (vo->driver->flip_page_timed)
        vo->driver->flip_page_timed(vo, 0, -1);
    else
        vo->driver->flip_page(vo);
}

static void *vo_thread(void *ptr)
{
    struct vo *vo = ptr;
    struct vo_internal *in = vo->in;

    int r = vo->driver->preinit(vo) ? -1 : 0;
    mp_rendezvous(vo, r); // init barrier
    if (r < 0)
        return NULL;

    while (1) {
        mp_dispatch_queue_process(vo->in->dispatch, 0);
        if (in->terminate)
            break;
        vo->driver->control(vo, VOCTRL_CHECK_EVENTS, NULL);
        bool frame_shown = render_frame(vo);
        int64_t now = mp_time_us();
        int64_t wait_until = now + (frame_shown ? 0 : (int64_t)1e9);
        pthread_mutex_lock(&in->lock);
        if (in->wakeup_pts) {
            if (in->wakeup_pts > now) {
                wait_until = MPMIN(wait_until, in->wakeup_pts);
            } else {
                in->wakeup_pts = 0;
                mp_input_wakeup(vo->input_ctx);
            }
        }
        if (vo->want_redraw && !in->want_redraw) {
            in->want_redraw = true;
            mp_input_wakeup(vo->input_ctx);
        }
        bool redraw = in->request_redraw;
        pthread_mutex_unlock(&in->lock);
        if (wait_until > now && redraw) {
            do_redraw(vo); // now is a good time
            continue;
        }
        wait_vo(vo, wait_until);
    }
    forget_frames(vo); // implicitly synchronized
    vo->driver->uninit(vo);
    return NULL;
}

void vo_set_paused(struct vo *vo, bool paused)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    if (in->paused != paused) {
        in->paused = paused;
        if (in->paused && in->dropped_frame)
            in->request_redraw = true;
    }
    pthread_mutex_unlock(&in->lock);
    vo_control(vo, paused ? VOCTRL_PAUSE : VOCTRL_RESUME, NULL);
}

int64_t vo_get_drop_count(struct vo *vo)
{
    pthread_mutex_lock(&vo->in->lock);
    int64_t r = vo->in->drop_count;
    pthread_mutex_unlock(&vo->in->lock);
    return r;
}

// Make the VO redraw the OSD at some point in the future.
void vo_redraw(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    if (!in->request_redraw) {
        in->request_redraw = true;
        wakeup_locked(vo);
    }
    pthread_mutex_unlock(&in->lock);
}

bool vo_want_redraw(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    bool r = in->want_redraw;
    pthread_mutex_unlock(&in->lock);
    return r;
}

void vo_seek_reset(struct vo *vo)
{
    pthread_mutex_lock(&vo->in->lock);
    forget_frames(vo);
    pthread_mutex_unlock(&vo->in->lock);
    vo_control(vo, VOCTRL_RESET, NULL);
}

// Return true if there is still a frame being displayed (or queued).
// If this returns true, a wakeup some time in the future is guaranteed.
bool vo_still_displaying(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&vo->in->lock);
    int64_t now = mp_time_us();
    int64_t frame_end = in->frame_pts + MPMAX(in->frame_duration, 0);
    bool working = now < frame_end || in->rendering || in->frame_queued;
    pthread_mutex_unlock(&vo->in->lock);
    return working && in->hasframe;
}

// Whether at least 1 frame was queued or rendered since last seek or reconfig.
bool vo_has_frame(struct vo *vo)
{
    return vo->in->hasframe;
}

static void run_query_format(void *p)
{
    void **pp = p;
    struct vo *vo = pp[0];
    *(int *)pp[2] = vo->driver->query_format(vo, *(int *)pp[1]);
}

int vo_query_format(struct vo *vo, int format)
{
    int ret;
    void *p[] = {vo, &format, &ret};
    mp_dispatch_run(vo->in->dispatch, run_query_format, p);
    return ret;
}

// Calculate the appropriate source and destination rectangle to
// get a correctly scaled picture, including pan-scan.
// out_src: visible part of the video
// out_dst: area of screen covered by the video source rectangle
// out_osd: OSD size, OSD margins, etc.
// Must be called from the VO thread only.
void vo_get_src_dst_rects(struct vo *vo, struct mp_rect *out_src,
                          struct mp_rect *out_dst, struct mp_osd_res *out_osd)
{
    if (!vo->params) {
        *out_src = *out_dst = (struct mp_rect){0};
        *out_osd = (struct mp_osd_res){0};
        return;
    }
    mp_get_src_dst_rects(vo->log, vo->opts, vo->driver->caps, vo->params,
                         vo->dwidth, vo->dheight, vo->monitor_par,
                         out_src, out_dst, out_osd);
}

// Return the window title the VO should set. Always returns a null terminated
// string. The string is valid until frontend code is invoked again. Copy it if
// you need to keep the string for an extended period of time.
// Must be called from the VO thread only.
// Don't use for new code.
const char *vo_get_window_title(struct vo *vo)
{
    if (!vo->in->window_title)
        vo->in->window_title = talloc_strdup(vo, "");
    return vo->in->window_title;
}

// flip_page[_timed] will be called this many microseconds too early.
// (For vo_vdpau, which does its own timing.)
void vo_set_flip_queue_offset(struct vo *vo, int64_t us)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    in->flip_queue_offset = us;
    pthread_mutex_unlock(&in->lock);
}

int64_t vo_get_vsync_interval(struct vo *vo)
{
    return vo->in->vsync_interval;
}

/**
 * \brief lookup an integer in a table, table must have 0 as the last key
 * \param key key to search for
 * \result translation corresponding to key or "to" value of last mapping
 *         if not found.
 */
int lookup_keymap_table(const struct mp_keymap *map, int key)
{
    while (map->from && map->from != key)
        map++;
    return map->to;
}
