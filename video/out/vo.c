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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <math.h>

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
#include "sub/osd.h"
#include "osdep/io.h"
#include "osdep/threads.h"

extern const struct vo_driver video_out_x11;
extern const struct vo_driver video_out_vdpau;
extern const struct vo_driver video_out_xv;
extern const struct vo_driver video_out_opengl;
extern const struct vo_driver video_out_opengl_hq;
extern const struct vo_driver video_out_opengl_cb;
extern const struct vo_driver video_out_null;
extern const struct vo_driver video_out_image;
extern const struct vo_driver video_out_lavc;
extern const struct vo_driver video_out_caca;
extern const struct vo_driver video_out_drm;
extern const struct vo_driver video_out_direct3d;
extern const struct vo_driver video_out_direct3d_shaders;
extern const struct vo_driver video_out_sdl;
extern const struct vo_driver video_out_vaapi;
extern const struct vo_driver video_out_wayland;
extern const struct vo_driver video_out_rpi;

const struct vo_driver *const video_out_drivers[] =
{
#if HAVE_RPI
    &video_out_rpi,
#endif
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
#if HAVE_WAYLAND
    &video_out_wayland,
#endif
#if HAVE_XV
    &video_out_xv,
#endif
#if HAVE_SDL2
    &video_out_sdl,
#endif
#if HAVE_VAAPI_X11
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
#if HAVE_DRM
    &video_out_drm,
#endif
#if HAVE_ENCODING
    &video_out_lavc,
#endif
#if HAVE_GL
    &video_out_opengl_hq,
    &video_out_opengl_cb,
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
    bool send_reset;                // send VOCTRL_RESET
    bool paused;
    int queued_events;              // event mask for the user
    int internal_events;            // event mask for us

    int64_t nominal_vsync_interval;

    int64_t vsync_interval;
    int64_t *vsync_samples;
    int num_vsync_samples;
    int64_t num_total_vsync_samples;
    int64_t prev_vsync;
    int64_t base_vsync;
    int drop_point;
    double estimated_vsync_interval;
    double estimated_vsync_jitter;
    bool expecting_vsync;

    int64_t flip_queue_offset; // queue flip events at most this much in advance

    int64_t delayed_count;
    int64_t drop_count;
    bool dropped_frame;             // the previous frame was dropped

    struct vo_frame *current_frame; // last frame queued to the VO

    int64_t wakeup_pts;             // time at which to pull frame from decoder

    bool rendering;                 // true if an image is being rendered
    struct vo_frame *frame_queued;  // should be drawn next
    int req_frames;                 // VO's requested value of num_frames

    double display_fps;
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
        .hidden = vo->encode || !strcmp(vo->name, "opengl-cb"),
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

static struct vo *vo_create(bool probing, struct mpv_global *global,
                            struct vo_extra *ex, char *name, char **args)
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
        .encode_lavc_ctx = ex->encode_lavc_ctx,
        .input_ctx = ex->input_ctx,
        .osd = ex->osd,
        .event_fd = -1,
        .monitor_par = 1,
        .extra = *ex,
        .probing = probing,
        .in = talloc(vo, struct vo_internal),
    };
    talloc_steal(vo, log);
    *vo->in = (struct vo_internal) {
        .dispatch = mp_dispatch_create(vo),
        .req_frames = 1,
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

struct vo *init_best_video_out(struct mpv_global *global, struct vo_extra *ex)
{
    struct m_obj_settings *vo_list = global->opts->vo.video_driver_list;
    // first try the preferred drivers, with their optional subdevice param:
    if (vo_list && vo_list[0].name) {
        for (int n = 0; vo_list[n].name; n++) {
            // Something like "-vo name," allows fallback to autoprobing.
            if (strlen(vo_list[n].name) == 0)
                goto autoprobe;
            bool p = !!vo_list[n + 1].name;
            struct vo *vo = vo_create(p, global, ex, vo_list[n].name,
                                      vo_list[n].attribs);
            if (vo)
                return vo;
        }
        return NULL;
    }
autoprobe:
    // now try the rest...
    for (int i = 0; video_out_drivers[i]; i++) {
        const struct vo_driver *driver = video_out_drivers[i];
        if (driver == &video_out_null)
            break;
        struct vo *vo = vo_create(true, global, ex, (char *)driver->name, NULL);
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

// Drop timing information on discontinuities like seeking.
// Always called locked.
static void reset_vsync_timings(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    in->num_vsync_samples = 0;
    in->num_total_vsync_samples = 0;
    in->drop_point = 0;
    in->estimated_vsync_interval = 0;
    in->estimated_vsync_jitter = -1;
    in->base_vsync = 0;
    in->expecting_vsync = false;
}

static double vsync_stddef(struct vo *vo, int64_t ref_vsync)
{
    struct vo_internal *in = vo->in;
    double jitter = 0;
    for (int n = 0; n < in->num_vsync_samples; n++) {
        double diff = in->vsync_samples[n] - ref_vsync;
        jitter += diff * diff;
    }
    return sqrt(jitter / in->num_vsync_samples);
}

#define MAX_VSYNC_SAMPLES 200

// Check if we should switch to measured average display FPS if it seems
// "better" then the system-reported one. (Note that small differences are
// handled as drift instead.)
static void check_estimated_display_fps(struct vo *vo)
{
    struct vo_internal *in = vo->in;

    bool use_estimated = false;
    if (in->num_total_vsync_samples >= MAX_VSYNC_SAMPLES * 2 &&
        fabs((in->nominal_vsync_interval - in->estimated_vsync_interval))
            >= 0.01 * in->nominal_vsync_interval &&
        in->estimated_vsync_interval <= 1e6 / 20.0 &&
        in->estimated_vsync_interval >= 1e6 / 99.0)
    {
        for (int n = 0; n < in->num_vsync_samples; n++) {
            if (fabs(in->vsync_samples[n] - in->estimated_vsync_interval)
                >= in->estimated_vsync_interval / 4)
                goto done;
        }
        double mjitter = vsync_stddef(vo, in->estimated_vsync_interval);
        double njitter = vsync_stddef(vo, in->nominal_vsync_interval);
        if (mjitter * 1.01 < njitter)
            use_estimated = true;
        done: ;
    }
    if (use_estimated == (in->vsync_interval == in->nominal_vsync_interval)) {
        if (use_estimated) {
            MP_WARN(vo, "Reported display FPS seems incorrect.\n"
                        "Assuming a value closer to %.3f Hz.\n",
                        1e6 / in->estimated_vsync_interval);
        } else {
            MP_WARN(vo, "Switching back to assuming %.3f Hz.\n",
                    1e6 / in->nominal_vsync_interval);
        }
    }
    in->vsync_interval = use_estimated ? (int64_t)in->estimated_vsync_interval
                                       : in->nominal_vsync_interval;
}

// Attempt to detect vsyncs delayed/skipped by the driver. This tries to deal
// with strong jitter too, because some drivers have crap vsync timing.
static void vsync_skip_detection(struct vo *vo)
{
    struct vo_internal *in = vo->in;

    int window = 4;
    int64_t t_r = in->prev_vsync, t_e = in->base_vsync, diff = 0, desync_early = 0;
    for (int n = 0; n < in->drop_point; n++) {
        diff += t_r - t_e;
        t_r -= in->vsync_samples[n];
        t_e -= in->vsync_interval;
        if (n == window + 1)
            desync_early = diff / window;
    }
    int64_t desync = diff / in->num_vsync_samples;
    if (in->drop_point > window * 2 &&
        labs(desync - desync_early) >= in->vsync_interval * 3 / 4)
    {
        // Assume a drop. An underflow can technically speaking not be a drop
        // (it's up to the driver what this is supposed to mean), but no reason
        // to treat it differently.
        in->base_vsync = in->prev_vsync;
        in->delayed_count += 1;
        in->drop_point = 0;
        MP_STATS(vo, "vo-delayed");
    }
    if (in->drop_point > 10)
        in->base_vsync += desync / 10;  // smooth out drift
}

// Always called locked.
static void update_vsync_timing_after_swap(struct vo *vo)
{
    struct vo_internal *in = vo->in;

    int64_t now = mp_time_us();

    if (!in->expecting_vsync) {
        in->prev_vsync = now; // for normal system-time framedrop
        reset_vsync_timings(vo);
        return;
    }

    if (in->num_vsync_samples >= MAX_VSYNC_SAMPLES)
        in->num_vsync_samples -= 1;
    MP_TARRAY_INSERT_AT(in, in->vsync_samples, in->num_vsync_samples, 0,
                        now - in->prev_vsync);
    in->drop_point = MPMIN(in->drop_point + 1, in->num_vsync_samples);
    in->num_total_vsync_samples += 1;
    if (in->base_vsync) {
        in->base_vsync += in->vsync_interval;
    } else {
        in->base_vsync = now;
    }
    in->prev_vsync = now;

    double avg = 0;
    for (int n = 0; n < in->num_vsync_samples; n++)
        avg += in->vsync_samples[n];
    in->estimated_vsync_interval = avg / in->num_vsync_samples;
    in->estimated_vsync_jitter =
        vsync_stddef(vo, in->vsync_interval) / in->vsync_interval;

    check_estimated_display_fps(vo);
    vsync_skip_detection(vo);

    MP_STATS(vo, "value %f jitter", in->estimated_vsync_jitter);
    MP_STATS(vo, "value %f vsync-diff", in->vsync_samples[0] / 1e6);
}

// to be called from VO thread only
static void update_display_fps(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    if (in->internal_events & VO_EVENT_WIN_STATE) {
        in->internal_events &= ~(unsigned)VO_EVENT_WIN_STATE;

        pthread_mutex_unlock(&in->lock);

        double display_fps = 0;
        if (vo->global->opts->frame_drop_fps > 0) {
            display_fps = vo->global->opts->frame_drop_fps;
        } else {
            vo->driver->control(vo, VOCTRL_GET_DISPLAY_FPS, &display_fps);
        }

        pthread_mutex_lock(&in->lock);

        if (in->display_fps != display_fps) {
            in->display_fps = display_fps;
            MP_VERBOSE(vo, "Assuming %f FPS for display sync.\n", display_fps);

            // make sure to update the player
            in->queued_events |= VO_EVENT_WIN_STATE;
            mp_input_wakeup(vo->input_ctx);
        }

        in->nominal_vsync_interval = in->display_fps > 0 ? 1e6 / in->display_fps : 0;
        in->vsync_interval = MPMAX(in->nominal_vsync_interval, 1);
    }
    pthread_mutex_unlock(&in->lock);
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
    int *ret = pp[2];

    struct vo_internal *in = vo->in;

    vo->dwidth = params->d_w;
    vo->dheight = params->d_h;

    talloc_free(vo->params);
    vo->params = talloc_memdup(vo, params, sizeof(*params));

    *ret = vo->driver->reconfig(vo, vo->params);
    vo->config_ok = *ret >= 0;
    if (vo->config_ok) {
        check_vo_caps(vo);
    } else {
        talloc_free(vo->params);
        vo->params = NULL;
    }

    pthread_mutex_lock(&in->lock);
    talloc_free(in->current_frame);
    in->current_frame = NULL;
    forget_frames(vo);
    reset_vsync_timings(vo);
    pthread_mutex_unlock(&in->lock);

    update_display_fps(vo);
}

int vo_reconfig(struct vo *vo, struct mp_image_params *params)
{
    int ret;
    void *p[] = {vo, params, &ret};
    mp_dispatch_run(vo->in->dispatch, run_reconfig, p);
    return ret;
}

static void run_control(void *p)
{
    void **pp = p;
    struct vo *vo = pp[0];
    uint32_t request = *(int *)pp[1];
    void *data = pp[2];
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
    in->delayed_count = 0;
    talloc_free(in->frame_queued);
    in->frame_queued = NULL;
    // don't unref current_frame; we always want to be able to redraw it
    if (in->current_frame) {
        in->current_frame->num_vsyncs = 0; // but reset future repeats
        in->current_frame->display_synced = false; // mark discontinuity
    }
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
        if (!in->need_wakeup) {
            struct timespec ts = mp_time_us_to_timespec(until_time);
            pthread_cond_timedwait(&in->wakeup, &in->lock, &ts);
        }
        in->need_wakeup = false;
        pthread_mutex_unlock(&in->lock);
    }
}

static void wakeup_locked(struct vo *vo)
{
    struct vo_internal *in = vo->in;

    pthread_cond_broadcast(&in->wakeup);
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
// If next_pts is negative, disable any timing and draw the frame as fast as
// possible.
bool vo_is_ready_for_frame(struct vo *vo, int64_t next_pts)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    bool r = vo->config_ok && !in->frame_queued &&
             (!in->current_frame || in->current_frame->num_vsyncs < 1);
    if (r && next_pts >= 0) {
        // Don't show the frame too early - it would basically freeze the
        // display by disallowing OSD redrawing or VO interaction.
        // Actually render the frame at earliest 50ms before target time.
        next_pts -= (uint64_t)(0.050 * 1e6);
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
// Ownership of frame is handed to the vo.
void vo_queue_frame(struct vo *vo, struct vo_frame *frame)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    assert(vo->config_ok && !in->frame_queued &&
           (!in->current_frame || in->current_frame->num_vsyncs < 1));
    in->hasframe = true;
    in->frame_queued = frame;
    in->wakeup_pts = frame->display_synced
                   ? 0 : frame->pts + MPMAX(frame->duration, 0);
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

// Wait until realtime is >= ts
// called without lock
static void wait_until(struct vo *vo, int64_t target)
{
    struct vo_internal *in = vo->in;
    struct timespec ts = mp_time_us_to_timespec(target);
    pthread_mutex_lock(&in->lock);
    while (target > mp_time_us()) {
        if (in->queued_events & VO_EVENT_LIVE_RESIZING)
            break;
        if (pthread_cond_timedwait(&in->wakeup, &in->lock, &ts))
            break;
    }
    pthread_mutex_unlock(&in->lock);
}

static bool render_frame(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    struct vo_frame *frame = NULL;
    bool got_frame = false;

    update_display_fps(vo);

    pthread_mutex_lock(&in->lock);

    if (in->frame_queued) {
        talloc_free(in->current_frame);
        in->current_frame = in->frame_queued;
        in->frame_queued = NULL;
    } else if (in->paused || !in->current_frame || !in->hasframe ||
               (in->current_frame->display_synced && in->current_frame->num_vsyncs < 1) ||
               !in->current_frame->display_synced)
    {
        goto done;
    }

    frame = vo_frame_ref(in->current_frame);
    assert(frame);

    if (frame->display_synced) {
        frame->pts = 0;
        frame->duration = -1;
    }

    int64_t now = mp_time_us();
    int64_t pts = frame->pts;
    int64_t duration = frame->duration;
    int64_t end_time = pts + duration;

    // Time at which we should flip_page on the VO.
    int64_t target = frame->display_synced ? 0 : pts - in->flip_queue_offset;

    // "normal" strict drop threshold.
    in->dropped_frame = duration >= 0 && end_time < now;

    in->dropped_frame &= !frame->display_synced;
    in->dropped_frame &= !(vo->driver->caps & VO_CAP_FRAMEDROP);
    in->dropped_frame &= (vo->global->opts->frame_dropping & 1);
    // Even if we're hopelessly behind, rather degrade to 10 FPS playback,
    // instead of just freezing the display forever.
    in->dropped_frame &= now - in->prev_vsync < 100 * 1000;
    in->dropped_frame &= in->hasframe_rendered;

    // Setup parameters for the next time this frame is drawn. ("frame" is the
    // frame currently drawn, while in->current_frame is the potentially next.)
    in->current_frame->repeat = true;
    if (frame->display_synced) {
        in->current_frame->vsync_offset += in->current_frame->vsync_interval;
        in->dropped_frame |= in->current_frame->num_vsyncs < 1;
    }
    if (in->current_frame->num_vsyncs > 0)
        in->current_frame->num_vsyncs -= 1;

    in->expecting_vsync = in->current_frame->display_synced && !in->paused;
    if (in->expecting_vsync && !in->num_vsync_samples) // first DS frame in a row
        in->prev_vsync = now;

    if (in->dropped_frame) {
        in->drop_count += 1;
    } else {
        in->rendering = true;
        in->hasframe_rendered = true;
        int64_t prev_drop_count = vo->in->drop_count;
        pthread_mutex_unlock(&in->lock);
        mp_input_wakeup(vo->input_ctx); // core can queue new video now

        MP_STATS(vo, "start video");

        if (vo->driver->draw_frame) {
            vo->driver->draw_frame(vo, frame);
        } else {
            vo->driver->draw_image(vo, mp_image_new_ref(frame->current));
        }

        wait_until(vo, target);

        vo->driver->flip_page(vo);

        MP_STATS(vo, "end video");
        MP_STATS(vo, "video_end");

        pthread_mutex_lock(&in->lock);
        in->dropped_frame = prev_drop_count < vo->in->drop_count;
        in->rendering = false;

        update_vsync_timing_after_swap(vo);
    }

    if (!in->dropped_frame) {
        vo->want_redraw = false;
        in->want_redraw = false;
        in->request_redraw = false;
    }

    pthread_cond_broadcast(&in->wakeup); // for vo_wait_frame()
    mp_input_wakeup(vo->input_ctx);

    got_frame = true;

done:
    talloc_free(frame);
    pthread_mutex_unlock(&in->lock);
    return got_frame || (in->frame_queued && in->frame_queued->display_synced);
}

static void do_redraw(struct vo *vo)
{
    struct vo_internal *in = vo->in;

    vo->want_redraw = false;

    if (!vo->config_ok)
        return;

    pthread_mutex_lock(&in->lock);
    in->request_redraw = false;
    in->want_redraw = false;
    bool full_redraw = in->dropped_frame;
    struct vo_frame *frame = NULL;
    if (!vo->driver->untimed)
        frame = vo_frame_ref(in->current_frame);
    if (frame)
        in->dropped_frame = false;
    struct vo_frame dummy = {0};
    if (!frame)
        frame = &dummy;
    frame->redraw = !full_redraw; // unconditionally redraw if it was dropped
    frame->still = true;
    frame->pts = 0;
    frame->duration = -1;
    pthread_mutex_unlock(&in->lock);

    if (vo->driver->draw_frame) {
        vo->driver->draw_frame(vo, frame);
    } else if ((full_redraw || vo->driver->control(vo, VOCTRL_REDRAW_FRAME, NULL) < 1)
               && frame->current)
    {
        vo->driver->draw_image(vo, mp_image_new_ref(frame->current));
    }

    vo->driver->flip_page(vo);

    if (frame != &dummy)
        talloc_free(frame);
}

static void *vo_thread(void *ptr)
{
    struct vo *vo = ptr;
    struct vo_internal *in = vo->in;

    mpthread_set_name("vo");

    int r = vo->driver->preinit(vo) ? -1 : 0;
    mp_rendezvous(vo, r); // init barrier
    if (r < 0)
        return NULL;

    update_display_fps(vo);
    vo_event(vo, VO_EVENT_WIN_STATE);

    while (1) {
        mp_dispatch_queue_process(vo->in->dispatch, 0);
        if (in->terminate)
            break;
        vo->driver->control(vo, VOCTRL_CHECK_EVENTS, NULL);
        bool working = render_frame(vo);
        int64_t now = mp_time_us();
        int64_t wait_until = now + (working ? 0 : (int64_t)1e9);
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
        bool send_reset = in->send_reset;
        in->send_reset = false;
        pthread_mutex_unlock(&in->lock);
        if (send_reset)
            vo->driver->control(vo, VOCTRL_RESET, NULL);
        if (wait_until > now && redraw) {
            do_redraw(vo); // now is a good time
            continue;
        }
        wait_vo(vo, wait_until);
    }
    forget_frames(vo); // implicitly synchronized
    talloc_free(in->current_frame);
    in->current_frame = NULL;
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
        reset_vsync_timings(vo);
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

void vo_increment_drop_count(struct vo *vo, int64_t n)
{
    pthread_mutex_lock(&vo->in->lock);
    vo->in->drop_count += n;
    pthread_mutex_unlock(&vo->in->lock);
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
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    forget_frames(vo);
    reset_vsync_timings(vo);
    in->send_reset = true;
    wakeup_locked(vo);
    pthread_mutex_unlock(&in->lock);
}

// Return true if there is still a frame being displayed (or queued).
// If this returns true, a wakeup some time in the future is guaranteed.
bool vo_still_displaying(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&vo->in->lock);
    int64_t now = mp_time_us();
    int64_t frame_end = 0;
    if (in->current_frame) {
        frame_end = in->current_frame->pts + MPMAX(in->current_frame->duration, 0);
        if (in->current_frame->display_synced)
            frame_end = in->current_frame->num_vsyncs > 0 ? INT64_MAX : 0;
    }
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
    uint8_t *list = pp[1];
    for (int format = IMGFMT_START; format < IMGFMT_END; format++)
        list[format - IMGFMT_START] = vo->driver->query_format(vo, format);
}

// For each item in the list (allocated as uint8_t[IMGFMT_END - IMGFMT_START]),
// set the supported format flags.
void vo_query_formats(struct vo *vo, uint8_t *list)
{
    void *p[] = {vo, list};
    mp_dispatch_run(vo->in->dispatch, run_query_format, p);
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

// flip_page[_timed] will be called offset_us microseconds too early.
// (For vo_vdpau, which does its own timing.)
// num_req_frames set the requested number of requested vo_frame.frames.
// (For vo_opengl interpolation.)
void vo_set_queue_params(struct vo *vo, int64_t offset_us, int num_req_frames)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    in->flip_queue_offset = offset_us;
    in->req_frames = MPCLAMP(num_req_frames, 1, VO_MAX_REQ_FRAMES);
    pthread_mutex_unlock(&in->lock);
}

int vo_get_num_req_frames(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    int res = in->req_frames;
    pthread_mutex_unlock(&in->lock);
    return res;
}

int64_t vo_get_vsync_interval(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    int64_t res = vo->in->vsync_interval > 1 ? vo->in->vsync_interval : -1;
    pthread_mutex_unlock(&in->lock);
    return res;
}

// Returns duration of a display refresh in seconds.
double vo_get_estimated_vsync_interval(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    double res = in->estimated_vsync_interval / 1e6;
    pthread_mutex_unlock(&in->lock);
    return res;
}

double vo_get_estimated_vsync_jitter(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    double res = in->estimated_vsync_jitter;
    pthread_mutex_unlock(&in->lock);
    return res;
}

// Get the time in seconds at after which the currently rendering frame will
// end. Returns positive values if the frame is yet to be finished, negative
// values if it already finished.
// This can only be called while no new frame is queued (after
// vo_is_ready_for_frame). Returns 0 for non-display synced frames, or if the
// deadline for continuous display was missed.
double vo_get_delay(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    assert (!in->frame_queued);
    int64_t res = 0;
    if (in->base_vsync && in->vsync_interval > 1 && in->current_frame) {
        res = in->base_vsync;
        int extra = !!in->rendering;
        res += (in->current_frame->num_vsyncs + extra) * in->vsync_interval;
        if (!in->current_frame->display_synced)
            res = 0;
    }
    pthread_mutex_unlock(&in->lock);
    return res ? (res - mp_time_us()) / 1e6 : 0;
}

int64_t vo_get_delayed_count(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    int64_t res = vo->in->delayed_count;
    pthread_mutex_unlock(&in->lock);
    return res;
}

double vo_get_display_fps(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    double res = vo->in->display_fps;
    pthread_mutex_unlock(&in->lock);
    return res;
}

// Set specific event flags, and wakeup the playback core if needed.
// vo_query_and_reset_events() can retrieve the events again.
void vo_event(struct vo *vo, int event)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    if ((in->queued_events & event & VO_EVENTS_USER) != (event & VO_EVENTS_USER))
        mp_input_wakeup(vo->input_ctx);
    if (event)
        wakeup_locked(vo);
    in->queued_events |= event;
    in->internal_events |= event;
    pthread_mutex_unlock(&in->lock);
}

// Check event flags set with vo_event(). Return the mask of events that was
// set and included in the events parameter. Clear the returned events.
int vo_query_and_reset_events(struct vo *vo, int events)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    int r = in->queued_events & events;
    in->queued_events &= ~(unsigned)r;
    pthread_mutex_unlock(&in->lock);
    return r;
}

struct mp_image *vo_get_current_frame(struct vo *vo)
{
    struct vo_internal *in = vo->in;
    pthread_mutex_lock(&in->lock);
    struct mp_image *r = NULL;
    if (vo->in->current_frame)
        r = mp_image_new_ref(vo->in->current_frame->current);
    pthread_mutex_unlock(&in->lock);
    return r;
}

static void destroy_frame(void *p)
{
    struct vo_frame *frame = p;
    for (int n = 0; n < frame->num_frames; n++)
        talloc_free(frame->frames[n]);
}

// Return a new reference to the given frame. The image pointers are also new
// references. Calling talloc_free() on the frame unrefs all currently set
// image references. (Assuming current==frames[0].)
struct vo_frame *vo_frame_ref(struct vo_frame *frame)
{
    if (!frame)
        return NULL;

    struct vo_frame *new = talloc_ptrtype(NULL, new);
    talloc_set_destructor(new, destroy_frame);
    *new = *frame;
    for (int n = 0; n < frame->num_frames; n++) {
        new->frames[n] = mp_image_new_ref(frame->frames[n]);
        if (!new->frames[n])
            abort(); // OOM on tiny allocs
    }
    new->current = new->num_frames ? new->frames[0] : NULL;
    return new;
}

/*
 * lookup an integer in a table, table must have 0 as the last key
 * param: key key to search for
 * returns translation corresponding to key or "to" value of last mapping
 *         if not found.
 */
int lookup_keymap_table(const struct mp_keymap *map, int key)
{
    while (map->from && map->from != key)
        map++;
    return map->to;
}
