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

#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>
#include <assert.h>

#include "config.h"
#include "mpv_talloc.h"

#include "common/msg.h"
#include "options/options.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "common/common.h"
#include "common/encode.h"
#include "options/m_property.h"
#include "osdep/timer.h"

#include "audio/out/ao.h"
#include "demux/demux.h"
#include "stream/stream.h"
#include "sub/osd.h"
#include "video/hwdec.h"
#include "video/filter/vf.h"
#include "video/decode/dec_video.h"
#include "video/decode/vd.h"
#include "video/out/vo.h"
#include "audio/filter/af.h"
#include "audio/decode/dec_audio.h"

#include "core.h"
#include "command.h"
#include "screenshot.h"

enum {
    // update_video() - code also uses: <0 error, 0 eof, >0 progress
    VD_ERROR = -1,
    VD_EOF = 0,         // end of file - no new output
    VD_PROGRESS = 1,    // progress, but no output; repeat call with no waiting
    VD_NEW_FRAME = 2,   // the call produced a new frame
    VD_WAIT = 3,        // no EOF, but no output; wait until wakeup
    VD_RECONFIG = 4,
};

static const char av_desync_help_text[] =
"\n"
"Audio/Video desynchronisation detected! Possible reasons include too slow\n"
"hardware, temporary CPU spikes, broken drivers, and broken files. Audio\n"
"position will not match to the video (see A-V status field).\n"
"\n";

int video_set_colors(struct vo_chain *vo_c, const char *item, int value)
{
    vf_equalizer_t data;

    data.item = item;
    data.value = value;

    MP_VERBOSE(vo_c, "set video colors %s=%d \n", item, value);
    if (video_vf_vo_control(vo_c, VFCTRL_SET_EQUALIZER, &data) == CONTROL_TRUE)
        return 1;
    MP_VERBOSE(vo_c, "Video attribute '%s' is not supported by selected vo.\n",
               item);
    return 0;
}

int video_get_colors(struct vo_chain *vo_c, const char *item, int *value)
{
    vf_equalizer_t data;

    data.item = item;

    MP_VERBOSE(vo_c, "get video colors %s \n", item);
    if (video_vf_vo_control(vo_c, VFCTRL_GET_EQUALIZER, &data) == CONTROL_TRUE) {
        *value = data.value;
        return 1;
    }
    return 0;
}

// Send a VCTRL, or if it doesn't work, translate it to a VOCTRL and try the VO.
int video_vf_vo_control(struct vo_chain *vo_c, int vf_cmd, void *data)
{
    if (vo_c->vf->initialized > 0) {
        int r = vf_control_any(vo_c->vf, vf_cmd, data);
        if (r != CONTROL_UNKNOWN)
            return r;
    }

    switch (vf_cmd) {
    case VFCTRL_GET_DEINTERLACE:
        return vo_control(vo_c->vo, VOCTRL_GET_DEINTERLACE, data) == VO_TRUE;
    case VFCTRL_SET_DEINTERLACE:
        return vo_control(vo_c->vo, VOCTRL_SET_DEINTERLACE, data) == VO_TRUE;
    case VFCTRL_SET_EQUALIZER: {
        vf_equalizer_t *eq = data;
        if (!vo_c->vo->config_ok)
            return CONTROL_FALSE;                       // vo not configured?
        struct voctrl_set_equalizer_args param = {
            eq->item, eq->value
        };
        return vo_control(vo_c->vo, VOCTRL_SET_EQUALIZER, &param) == VO_TRUE;
    }
    case VFCTRL_GET_EQUALIZER: {
        vf_equalizer_t *eq = data;
        if (!vo_c->vo->config_ok)
            return CONTROL_FALSE;                       // vo not configured?
        struct voctrl_get_equalizer_args param = {
            eq->item, &eq->value
        };
        return vo_control(vo_c->vo, VOCTRL_GET_EQUALIZER, &param) == VO_TRUE;
    }
    }
    return CONTROL_UNKNOWN;
}

static void set_allowed_vo_formats(struct vo_chain *vo_c)
{
    vo_query_formats(vo_c->vo, vo_c->vf->allowed_output_formats);
}

static int try_filter(struct vo_chain *vo_c, struct mp_image_params params,
                      char *name, char *label, char **args)
{
    struct vf_instance *vf = vf_append_filter(vo_c->vf, name, args);
    if (!vf)
        return -1;

    vf->label = talloc_strdup(vf, label);

    if (vf_reconfig(vo_c->vf, &params) < 0) {
        vf_remove_filter(vo_c->vf, vf);
        // restore
        vf_reconfig(vo_c->vf, &params);
        return -1;
    }
    return 0;
}

// Reconfigure the filter chain according to the new input format.
static void filter_reconfig(struct vo_chain *vo_c)
{
    struct mp_image_params params = vo_c->input_format;
    if (!params.imgfmt)
        return;

    set_allowed_vo_formats(vo_c);

    if (vf_reconfig(vo_c->vf, &params) < 0)
        return;

    if (params.rotate && (params.rotate % 90 == 0)) {
        if (!(vo_c->vo->driver->caps & VO_CAP_ROTATE90)) {
            // Try to insert a rotation filter.
            char *args[] = {"angle", "auto", NULL};
            if (try_filter(vo_c, params, "rotate", "autorotate", args) >= 0) {
                params.rotate = 0;
            } else {
                MP_ERR(vo_c, "Can't insert rotation filter.\n");
            }
        }
    }

    if (params.stereo_in != params.stereo_out &&
        params.stereo_in > 0 && params.stereo_out >= 0)
    {
        char *to = (char *)MP_STEREO3D_NAME(params.stereo_out);
        if (to) {
            char *args[] = {"in", "auto", "out", to, NULL, NULL};
            if (try_filter(vo_c, params, "stereo3d", "stereo3d", args) < 0)
                MP_ERR(vo_c, "Can't insert 3D conversion filter.\n");
        }
    }
}

static void recreate_video_filters(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    struct vo_chain *vo_c = mpctx->vo_chain;
    assert(vo_c);

    vf_destroy(vo_c->vf);
    vo_c->vf = vf_new(mpctx->global);
    vo_c->vf->hwdec = vo_c->hwdec_info;
    vo_c->vf->wakeup_callback = wakeup_playloop;
    vo_c->vf->wakeup_callback_ctx = mpctx;
    vo_c->vf->container_fps = vo_c->container_fps;
    vo_control(vo_c->vo, VOCTRL_GET_DISPLAY_FPS, &vo_c->vf->display_fps);

    vf_append_filter_list(vo_c->vf, opts->vf_settings);

    // for vf_sub
    osd_set_render_subs_in_filter(mpctx->osd,
        vf_control_any(vo_c->vf, VFCTRL_INIT_OSD, mpctx->osd) > 0);

    set_allowed_vo_formats(vo_c);
}

int reinit_video_filters(struct MPContext *mpctx)
{
    struct vo_chain *vo_c = mpctx->vo_chain;

    if (!vo_c)
        return 0;
    bool need_reconfig = vo_c->vf->initialized != 0;

    recreate_video_filters(mpctx);

    if (need_reconfig)
        filter_reconfig(vo_c);

    mp_force_video_refresh(mpctx);

    mp_notify(mpctx, MPV_EVENT_VIDEO_RECONFIG, NULL);

    return vo_c->vf->initialized;
}

static void vo_chain_reset_state(struct vo_chain *vo_c)
{
    mp_image_unrefp(&vo_c->input_mpi);
    if (vo_c->vf->initialized == 1)
        vf_seek_reset(vo_c->vf);
    vo_seek_reset(vo_c->vo);

    if (vo_c->video_src)
        video_reset(vo_c->video_src);
}

void reset_video_state(struct MPContext *mpctx)
{
    if (mpctx->vo_chain)
        vo_chain_reset_state(mpctx->vo_chain);

    for (int n = 0; n < mpctx->num_next_frames; n++)
        mp_image_unrefp(&mpctx->next_frames[n]);
    mpctx->num_next_frames = 0;
    mp_image_unrefp(&mpctx->saved_frame);

    mpctx->delay = 0;
    mpctx->time_frame = 0;
    mpctx->video_pts = MP_NOPTS_VALUE;
    mpctx->num_past_frames = 0;
    mpctx->total_avsync_change = 0;
    mpctx->last_av_difference = 0;
    mpctx->dropped_frames_start = 0;
    mpctx->mistimed_frames_total = 0;
    mpctx->drop_message_shown = 0;
    mpctx->display_sync_drift_dir = 0;
    mpctx->display_sync_broken = false;

    mpctx->video_status = mpctx->vo_chain ? STATUS_SYNCING : STATUS_EOF;
}

void uninit_video_out(struct MPContext *mpctx)
{
    uninit_video_chain(mpctx);
    if (mpctx->video_out) {
        vo_destroy(mpctx->video_out);
        mp_notify(mpctx, MPV_EVENT_VIDEO_RECONFIG, NULL);
    }
    mpctx->video_out = NULL;
}

static void vo_chain_uninit(struct vo_chain *vo_c)
{
    struct track *track = vo_c->track;
    if (track) {
        assert(track->vo_c == vo_c);
        track->vo_c = NULL;
        assert(track->d_video == vo_c->video_src);
        track->d_video = NULL;
        video_uninit(vo_c->video_src);
    }

    if (vo_c->filter_src)
        lavfi_set_connected(vo_c->filter_src, false);

    mp_image_unrefp(&vo_c->input_mpi);
    vf_destroy(vo_c->vf);
    talloc_free(vo_c);
    // this does not free the VO
}

void uninit_video_chain(struct MPContext *mpctx)
{
    if (mpctx->vo_chain) {
        reset_video_state(mpctx);
        vo_chain_uninit(mpctx->vo_chain);
        mpctx->vo_chain = NULL;

        mpctx->video_status = STATUS_EOF;

        remove_deint_filter(mpctx);
        mp_notify(mpctx, MPV_EVENT_VIDEO_RECONFIG, NULL);
    }
}

int init_video_decoder(struct MPContext *mpctx, struct track *track)
{
    assert(!track->d_video);
    if (!track->stream)
        goto err_out;

    track->d_video = talloc_zero(NULL, struct dec_video);
    struct dec_video *d_video = track->d_video;
    d_video->global = mpctx->global;
    d_video->log = mp_log_new(d_video, mpctx->log, "!vd");
    d_video->opts = mpctx->opts;
    d_video->header = track->stream;
    d_video->codec = track->stream->codec;
    d_video->fps = d_video->header->codec->fps;
    if (mpctx->vo_chain)
        d_video->hwdec_info = mpctx->vo_chain->hwdec_info;

    MP_VERBOSE(d_video, "Container reported FPS: %f\n", d_video->fps);

    if (d_video->opts->force_fps) {
        d_video->fps = d_video->opts->force_fps;
        MP_INFO(mpctx, "FPS forced to %5.3f.\n", d_video->fps);
        MP_INFO(mpctx, "Use --no-correct-pts to force FPS based timing.\n");
    }

    if (!video_init_best_codec(d_video))
        goto err_out;

    return 1;

err_out:
    if (track->sink)
        lavfi_set_connected(track->sink, false);
    track->sink = NULL;
    video_uninit(track->d_video);
    track->d_video = NULL;
    error_on_track(mpctx, track);
    return 0;
}

int reinit_video_chain(struct MPContext *mpctx)
{
    return reinit_video_chain_src(mpctx, NULL);
}

int reinit_video_chain_src(struct MPContext *mpctx, struct lavfi_pad *src)
{
    struct MPOpts *opts = mpctx->opts;
    struct track *track = NULL;
    struct sh_stream *sh = NULL;
    if (!src) {
        track = mpctx->current_track[0][STREAM_VIDEO];
        if (!track)
            return 0;
        sh = track->stream;
        if (!sh)
            goto no_video;
    }
    assert(!mpctx->vo_chain);

    if (!mpctx->video_out) {
        struct vo_extra ex = {
            .input_ctx = mpctx->input,
            .osd = mpctx->osd,
            .encode_lavc_ctx = mpctx->encode_lavc_ctx,
            .opengl_cb_context = mpctx->gl_cb_ctx,
        };
        mpctx->video_out = init_best_video_out(mpctx->global, &ex);
        if (!mpctx->video_out) {
            MP_FATAL(mpctx, "Error opening/initializing "
                    "the selected video_out (-vo) device.\n");
            mpctx->error_playing = MPV_ERROR_VO_INIT_FAILED;
            goto err_out;
        }
        mpctx->mouse_cursor_visible = true;
    }

    update_window_title(mpctx, true);

    struct vo_chain *vo_c = talloc_zero(NULL, struct vo_chain);
    mpctx->vo_chain = vo_c;
    vo_c->log = mpctx->log;
    vo_c->vo = mpctx->video_out;
    vo_c->vf = vf_new(mpctx->global);

    vo_control(vo_c->vo, VOCTRL_GET_HWDEC_INFO, &vo_c->hwdec_info);

    vo_c->filter_src = src;
    if (!vo_c->filter_src) {
        vo_c->track = track;
        track->vo_c = vo_c;
        if (!init_video_decoder(mpctx, track))
            goto err_out;

        vo_c->video_src = track->d_video;
        vo_c->container_fps = vo_c->video_src->fps;
        vo_c->is_coverart = !!sh->attached_picture;

        track->vo_c = vo_c;
        vo_c->track = track;
    }

#if HAVE_ENCODING
    if (mpctx->encode_lavc_ctx)
        encode_lavc_set_video_fps(mpctx->encode_lavc_ctx, vo_c->container_fps);
#endif

    recreate_video_filters(mpctx);

    bool saver_state = opts->pause || !opts->stop_screensaver;
    vo_control(vo_c->vo, saver_state ? VOCTRL_RESTORE_SCREENSAVER
                                     : VOCTRL_KILL_SCREENSAVER, NULL);

    vo_set_paused(vo_c->vo, mpctx->paused);

    // If we switch on video again, ensure audio position matches up.
    if (mpctx->ao_chain)
        mpctx->audio_status = STATUS_SYNCING;

    reset_video_state(mpctx);
    reset_subtitle_state(mpctx);

    return 1;

err_out:
no_video:
    uninit_video_chain(mpctx);
    error_on_track(mpctx, track);
    handle_force_window(mpctx, true);
    return 0;
}

// Try to refresh the video by doing a precise seek to the currently displayed
// frame. This can go wrong in all sorts of ways, so use sparingly.
void mp_force_video_refresh(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    struct vo_chain *vo_c = mpctx->vo_chain;

    if (!vo_c || !vo_c->input_format.imgfmt)
        return;

    // If not paused, the next frame should come soon enough.
    if (opts->pause && mpctx->video_status == STATUS_PLAYING &&
        mpctx->last_vo_pts != MP_NOPTS_VALUE)
    {
        queue_seek(mpctx, MPSEEK_ABSOLUTE, mpctx->last_vo_pts,
                   MPSEEK_VERY_EXACT, true);
    }
}

static bool check_framedrop(struct MPContext *mpctx, struct vo_chain *vo_c)
{
    struct MPOpts *opts = mpctx->opts;
    // check for frame-drop:
    if (mpctx->video_status == STATUS_PLAYING && !mpctx->paused &&
        mpctx->audio_status == STATUS_PLAYING && !ao_untimed(mpctx->ao) &&
        vo_c->video_src)
    {
        float fps = vo_c->container_fps;
        double frame_time = fps > 0 ? 1.0 / fps : 0;
        // we should avoid dropping too many frames in sequence unless we
        // are too late. and we allow 100ms A-V delay here:
        int dropped_frames =
            vo_c->video_src->dropped_frames - mpctx->dropped_frames_start;
        if (mpctx->last_av_difference - 0.100 > dropped_frames * frame_time)
            return !!(opts->frame_dropping & 2);
    }
    return false;
}

// Read a packet, store decoded image into d_video->waiting_decoded_mpi
// returns VD_* code
static int decode_image(struct MPContext *mpctx)
{
    struct vo_chain *vo_c = mpctx->vo_chain;
    if (vo_c->input_mpi)
        return VD_PROGRESS;

    int res = DATA_EOF;
    if (vo_c->filter_src) {
        res = lavfi_request_frame_v(vo_c->filter_src, &vo_c->input_mpi);
    } else if (vo_c->video_src) {
        struct dec_video *d_video = vo_c->video_src;
        bool hrseek = mpctx->hrseek_active && mpctx->hrseek_framedrop &&
                      mpctx->video_status == STATUS_SYNCING;
        video_set_start(d_video, hrseek ? mpctx->hrseek_pts : MP_NOPTS_VALUE);

        video_set_framedrop(d_video, check_framedrop(mpctx, vo_c));

        video_work(d_video);
        res = video_get_frame(d_video, &vo_c->input_mpi);
    }

    switch (res) {
    case DATA_WAIT:     return VD_WAIT;
    case DATA_OK:
    case DATA_AGAIN:    return VD_PROGRESS;
    case DATA_EOF:      return VD_EOF;
    default:            abort();
    }
}

// Called after video reinit. This can be generally used to try to insert more
// filters using the filter chain edit functionality in command.c.
static void init_filter_params(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;

    // Note that the filter chain is already initialized. This code might
    // recreate the chain a second time, which is not very elegant, but allows
    // us to test whether enabling deinterlacing works with the current video
    // format and other filters.
    if (opts->deinterlace >= 0) {
        remove_deint_filter(mpctx);
        set_deinterlacing(mpctx, opts->deinterlace != 0);
    }
}

// Feed newly decoded frames to the filter, take care of format changes.
// If eof=true, drain the filter chain, and return VD_EOF if empty.
static int video_filter(struct MPContext *mpctx, bool eof)
{
    struct vo_chain *vo_c = mpctx->vo_chain;
    struct vf_chain *vf = vo_c->vf;

    if (vf->initialized < 0)
        return VD_ERROR;

    // There is already a filtered frame available.
    // If vf_needs_input() returns > 0, the filter wants input anyway.
    if (vf_output_frame(vf, eof) > 0 && vf_needs_input(vf) < 1)
        return VD_PROGRESS;

    // Decoder output is different from filter input?
    bool need_vf_reconfig = !vf->input_params.imgfmt || vf->initialized < 1 ||
        !mp_image_params_equal(&vo_c->input_format, &vf->input_params);

    // (If imgfmt==0, nothing was decoded yet, and the format is unknown.)
    if (need_vf_reconfig && vo_c->input_format.imgfmt) {
        // Drain the filter chain.
        if (vf_output_frame(vf, true) > 0)
            return VD_PROGRESS;

        // The filter chain is drained; execute the filter format change.
        filter_reconfig(mpctx->vo_chain);

        mp_notify(mpctx, MPV_EVENT_VIDEO_RECONFIG, NULL);

        // Most video filters don't work with hardware decoding, so this
        // might be the reason why filter reconfig failed.
        if (vf->initialized < 0 && vo_c->video_src &&
            video_vd_control(vo_c->video_src, VDCTRL_FORCE_HWDEC_FALLBACK, NULL)
                == CONTROL_OK)
        {
            // Fallback active; decoder will return software format next
            // time. Don't abort video decoding.
            vf->initialized = 0;
            mp_image_unrefp(&vo_c->input_mpi);
            vo_c->input_format = (struct mp_image_params){0};
            MP_VERBOSE(mpctx, "hwdec falback due to filters.\n");
            return VD_PROGRESS; // try again
        }
        if (vf->initialized < 1) {
            MP_FATAL(mpctx, "Cannot initialize video filters.\n");
            return VD_ERROR;
        }
        init_filter_params(mpctx);
        return VD_RECONFIG;
    }

    // If something was decoded, and the filter chain is ready, filter it.
    if (!need_vf_reconfig && vo_c->input_mpi) {
        vf_filter_frame(vf, vo_c->input_mpi);
        vo_c->input_mpi = NULL;
        return VD_PROGRESS;
    }

    return eof ? VD_EOF : VD_PROGRESS;
}

// Make sure at least 1 filtered image is available, decode new video if needed.
// returns VD_* code
// A return value of VD_PROGRESS doesn't necessarily output a frame, but makes
// the promise that calling this function again will eventually do something.
static int video_decode_and_filter(struct MPContext *mpctx)
{
    struct vo_chain *vo_c = mpctx->vo_chain;

    int r = video_filter(mpctx, false);
    if (r < 0)
        return r;

    if (!vo_c->input_mpi) {
        // Decode a new image, or at least feed the decoder a packet.
        r = decode_image(mpctx);
        if (r == VD_WAIT)
            return r;
    }
    if (vo_c->input_mpi)
        vo_c->input_format = vo_c->input_mpi->params;

    bool eof = !vo_c->input_mpi && (r == VD_EOF || r < 0);
    r = video_filter(mpctx, eof);
    if (r == VD_RECONFIG) // retry feeding decoded image
        r = video_filter(mpctx, eof);
    return r;
}

static int video_feed_async_filter(struct MPContext *mpctx)
{
    struct vf_chain *vf = mpctx->vo_chain->vf;

    if (vf->initialized < 0)
        return VD_ERROR;

    if (vf_needs_input(vf) < 1)
        return 0;
    mpctx->sleeptime = 0; // retry until done
    return video_decode_and_filter(mpctx);
}

/* Modify video timing to match the audio timeline. There are two main
 * reasons this is needed. First, video and audio can start from different
 * positions at beginning of file or after a seek (MPlayer starts both
 * immediately even if they have different pts). Second, the file can have
 * audio timestamps that are inconsistent with the duration of the audio
 * packets, for example two consecutive timestamp values differing by
 * one second but only a packet with enough samples for half a second
 * of playback between them.
 */
static void adjust_sync(struct MPContext *mpctx, double v_pts, double frame_time)
{
    struct MPOpts *opts = mpctx->opts;

    if (mpctx->audio_status != STATUS_PLAYING)
        return;

    double a_pts = written_audio_pts(mpctx) + opts->audio_delay - mpctx->delay;
    double av_delay = a_pts - v_pts;

    double change = av_delay * 0.1;
    double max_change = opts->default_max_pts_correction >= 0 ?
                        opts->default_max_pts_correction : frame_time * 0.1;
    if (change < -max_change)
        change = -max_change;
    else if (change > max_change)
        change = max_change;
    mpctx->delay += change;
    mpctx->total_avsync_change += change;

    if (mpctx->display_sync_active)
        mpctx->total_avsync_change = 0;
}

// Make the frame at position 0 "known" to the playback logic. This must happen
// only once for each frame, so this function has to be called carefully.
// Generally, if position 0 gets a new frame, this must be called.
static void handle_new_frame(struct MPContext *mpctx)
{
    assert(mpctx->num_next_frames >= 1);

    double frame_time = 0;
    double pts = mpctx->next_frames[0]->pts;
    if (mpctx->video_pts != MP_NOPTS_VALUE) {
        frame_time = pts - mpctx->video_pts;
        double tolerance = 15;
        if (frame_time <= 0 || frame_time >= tolerance) {
            // Assume a discontinuity.
            MP_WARN(mpctx, "Invalid video timestamp: %f -> %f\n",
                    mpctx->video_pts, pts);
            frame_time = 0;
        }
    }
    mpctx->delay -= frame_time;
    if (mpctx->video_status >= STATUS_PLAYING) {
        mpctx->time_frame += frame_time / mpctx->video_speed;
        adjust_sync(mpctx, pts, frame_time);
    }
    struct dec_video *d_video = mpctx->vo_chain->video_src;
    if (d_video)
        mpctx->dropped_frames_start = d_video->dropped_frames;
    MP_TRACE(mpctx, "frametime=%5.3f\n", frame_time);
}

// Remove the first frame in mpctx->next_frames
static void shift_frames(struct MPContext *mpctx)
{
    if (mpctx->num_next_frames < 1)
        return;
    talloc_free(mpctx->next_frames[0]);
    for (int n = 0; n < mpctx->num_next_frames - 1; n++)
        mpctx->next_frames[n] = mpctx->next_frames[n + 1];
    mpctx->num_next_frames -= 1;
}

static int get_req_frames(struct MPContext *mpctx, bool eof)
{
    // On EOF, drain all frames.
    if (eof)
        return 1;

    // On the first frame, output a new frame as quickly as possible.
    // But display-sync likes to have a correct frame duration always.
    if (mpctx->video_pts == MP_NOPTS_VALUE)
        return mpctx->opts->video_sync == VS_DEFAULT ? 1 : 2;

    int req = vo_get_num_req_frames(mpctx->video_out);
    return MPCLAMP(req, 2, MP_ARRAY_SIZE(mpctx->next_frames) - 1);
}

// Whether it's fine to call add_new_frame() now.
static bool needs_new_frame(struct MPContext *mpctx)
{
    return mpctx->num_next_frames < get_req_frames(mpctx, false);
}

// Queue a frame to mpctx->next_frames[]. Call only if needs_new_frame() signals ok.
static void add_new_frame(struct MPContext *mpctx, struct mp_image *frame)
{
    assert(mpctx->num_next_frames < MP_ARRAY_SIZE(mpctx->next_frames));
    assert(frame);
    mpctx->next_frames[mpctx->num_next_frames++] = frame;
    if (mpctx->num_next_frames == 1)
        handle_new_frame(mpctx);
}

// Enough video filtered already to push one frame to the VO?
// Set eof to true if no new frames are to be expected.
static bool have_new_frame(struct MPContext *mpctx, bool eof)
{
    return mpctx->num_next_frames >= get_req_frames(mpctx, eof);
}

// Fill mpctx->next_frames[] with a newly filtered or decoded image.
// returns VD_* code
static int video_output_image(struct MPContext *mpctx)
{
    struct vo_chain *vo_c = mpctx->vo_chain;
    bool hrseek = mpctx->hrseek_active && mpctx->video_status == STATUS_SYNCING;

    if (vo_c->is_coverart) {
        if (vo_has_frame(mpctx->video_out))
            return VD_EOF;
        hrseek = false;
    }

    if (have_new_frame(mpctx, false))
        return VD_NEW_FRAME;

    // Get a new frame if we need one.
    int r = VD_PROGRESS;
    if (needs_new_frame(mpctx)) {
        // Filter a new frame.
        r = video_decode_and_filter(mpctx);
        if (r < 0)
            return r; // error
        struct mp_image *img = vf_read_output_frame(vo_c->vf);
        if (img) {
            double endpts = get_play_end_pts(mpctx);
            if (endpts != MP_NOPTS_VALUE && img->pts >= endpts) {
                r = VD_EOF;
            } else if (mpctx->max_frames == 0) {
                r = VD_EOF;
            } else if (hrseek && mpctx->hrseek_lastframe) {
                mp_image_setrefp(&mpctx->saved_frame, img);
            } else if (hrseek && img->pts < mpctx->hrseek_pts - .005) {
                /* just skip - but save if backstep active */
                if (mpctx->hrseek_backstep)
                    mp_image_setrefp(&mpctx->saved_frame, img);
            } else if (mpctx->video_status == STATUS_SYNCING &&
                       mpctx->playback_pts != MP_NOPTS_VALUE &&
                       img->pts < mpctx->playback_pts && !vo_c->is_coverart)
            {
                /* skip after stream-switching */
            } else {
                if (hrseek && mpctx->hrseek_backstep) {
                    if (mpctx->saved_frame) {
                        add_new_frame(mpctx, mpctx->saved_frame);
                        mpctx->saved_frame = NULL;
                    } else {
                        MP_WARN(mpctx, "Backstep failed.\n");
                    }
                    mpctx->hrseek_backstep = false;
                }
                add_new_frame(mpctx, img);
                img = NULL;
            }
            talloc_free(img);
        }
    }

    // Last-frame seek
    if (r <= 0 && hrseek && mpctx->hrseek_lastframe && mpctx->saved_frame) {
        add_new_frame(mpctx, mpctx->saved_frame);
        mpctx->saved_frame = NULL;
        r = VD_PROGRESS;
    }

    return have_new_frame(mpctx, r <= 0) ? VD_NEW_FRAME : r;
}

/* Update avsync before a new video frame is displayed. Actually, this can be
 * called arbitrarily often before the actual display.
 * This adjusts the time of the next video frame */
static void update_avsync_before_frame(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    struct vo *vo = mpctx->video_out;

    if (mpctx->vo_chain->is_coverart || mpctx->video_status < STATUS_READY) {
        mpctx->time_frame = 0;
    } else if (mpctx->display_sync_active || opts->video_sync == VS_NONE) {
        // don't touch the timing
    } else if (mpctx->audio_status == STATUS_PLAYING &&
               mpctx->video_status == STATUS_PLAYING &&
               !ao_untimed(mpctx->ao))
    {
        double buffered_audio = ao_get_delay(mpctx->ao);

        double predicted = mpctx->delay / mpctx->video_speed +
                           mpctx->time_frame;
        double difference = buffered_audio - predicted;
        MP_STATS(mpctx, "value %f audio-diff", difference);

        if (opts->autosync) {
            /* Smooth reported playback position from AO by averaging
             * it with the value expected based on previus value and
             * time elapsed since then. May help smooth video timing
             * with audio output that have inaccurate position reporting.
             * This is badly implemented; the behavior of the smoothing
             * now undesirably depends on how often this code runs
             * (mainly depends on video frame rate). */
            buffered_audio = predicted + difference / opts->autosync;
        }

        mpctx->time_frame = buffered_audio - mpctx->delay / mpctx->video_speed;
    } else {
        /* If we're more than 200 ms behind the right playback
         * position, don't try to speed up display of following
         * frames to catch up; continue with default speed from
         * the current frame instead.
         * If untimed is set always output frames immediately
         * without sleeping.
         */
        if (mpctx->time_frame < -0.2 || opts->untimed || vo->driver->untimed)
            mpctx->time_frame = 0;
    }
}

// Update the A/V sync difference when a new video frame is being shown.
static void update_av_diff(struct MPContext *mpctx, double offset)
{
    struct MPOpts *opts = mpctx->opts;

    mpctx->last_av_difference = 0;

    if (mpctx->audio_status != STATUS_PLAYING ||
        mpctx->video_status != STATUS_PLAYING)
        return;

    double a_pos = playing_audio_pts(mpctx);
    if (a_pos != MP_NOPTS_VALUE && mpctx->video_pts != MP_NOPTS_VALUE) {
        mpctx->last_av_difference = a_pos - mpctx->video_pts
                                  + opts->audio_delay + offset;
    }

    if (fabs(mpctx->last_av_difference) > 0.5 && !mpctx->drop_message_shown) {
        MP_WARN(mpctx, "%s", av_desync_help_text);
        mpctx->drop_message_shown = true;
    }
}

static void init_vo(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    struct vo_chain *vo_c = mpctx->vo_chain;

    if (opts->gamma_gamma != 1000)
        video_set_colors(vo_c, "gamma", opts->gamma_gamma);
    if (opts->gamma_brightness != 1000)
        video_set_colors(vo_c, "brightness", opts->gamma_brightness);
    if (opts->gamma_contrast != 1000)
        video_set_colors(vo_c, "contrast", opts->gamma_contrast);
    if (opts->gamma_saturation != 1000)
        video_set_colors(vo_c, "saturation", opts->gamma_saturation);
    if (opts->gamma_hue != 1000)
        video_set_colors(vo_c, "hue", opts->gamma_hue);
    video_set_colors(vo_c, "output-levels", opts->video_output_levels);

    mp_notify(mpctx, MPV_EVENT_VIDEO_RECONFIG, NULL);
}

double calc_average_frame_duration(struct MPContext *mpctx)
{
    double total = 0;
    int num = 0;
    for (int n = 0; n < mpctx->num_past_frames; n++) {
        double dur = mpctx->past_frames[0].approx_duration;
        if (dur <= 0)
            continue;
        total += dur;
        num += 1;
    }
    return num > 0 ? total / num : 0;
}

// Find a speed factor such that the display FPS is an integer multiple of the
// effective video FPS. If this is not possible, try to do it for multiples,
// which still leads to an improved end result.
// Both parameters are durations in seconds.
static double calc_best_speed(double vsync, double frame)
{
    double ratio = frame / vsync;
    double best_scale = -1;
    double best_dev = INFINITY;
    for (int factor = 1; factor <= 5; factor++) {
        double scale = ratio * factor / rint(ratio * factor);
        double dev = fabs(scale - 1);
        if (dev < best_dev) {
            best_scale = scale;
            best_dev = dev;
        }
    }
    return best_scale;
}

static double find_best_speed(struct MPContext *mpctx, double vsync)
{
    double total = 0;
    int num = 0;
    for (int n = 0; n < mpctx->num_past_frames; n++) {
        double dur = mpctx->past_frames[n].approx_duration;
        if (dur <= 0)
            continue;
        total += calc_best_speed(vsync, dur / mpctx->opts->playback_speed);
        num++;
    }
    return num > 0 ? total / num : 1;
}

static bool using_spdif_passthrough(struct MPContext *mpctx)
{
    if (mpctx->ao_chain)
        return !af_fmt_is_pcm(mpctx->ao_chain->input_format.format);
    return false;
}

// Compute the relative audio speed difference by taking A/V dsync into account.
static double compute_audio_drift(struct MPContext *mpctx, double vsync)
{
    // Least-squares linear regression, using relative real time for x, and
    // audio desync for y. Assume speed didn't change for the frames we're
    // looking at for simplicity. This also should actually use the realtime
    // (minus paused time) for x, but use vsync scheduling points instead.
    if (mpctx->num_past_frames <= 10)
        return NAN;
    int num = mpctx->num_past_frames - 1;
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    double x = 0;
    for (int n = 0; n < num; n++) {
        struct frame_info *frame = &mpctx->past_frames[n + 1];
        if (frame->num_vsyncs < 0)
            return NAN;
        double y = frame->av_diff;
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
        x -= frame->num_vsyncs * vsync;
    }
    return (sum_x * sum_y - num * sum_xy) / (sum_x * sum_x - num * sum_xx);
}

static void adjust_audio_resample_speed(struct MPContext *mpctx, double vsync)
{
    struct MPOpts *opts = mpctx->opts;
    int mode = opts->video_sync;

    if (mode != VS_DISP_RESAMPLE || mpctx->audio_status != STATUS_PLAYING) {
        mpctx->speed_factor_a = mpctx->speed_factor_v;
        return;
    }

    // Try to smooth out audio timing drifts. This can happen if either
    // video isn't playing at expected speed, or audio is not playing at
    // the requested speed. Both are unavoidable.
    // The audio desync is made up of 2 parts: 1. drift due to rounding
    // errors and imperfect information, and 2. an offset, due to
    // unaligned audio/video start, or disruptive events halting audio
    // or video for a small time.
    // Instead of trying to be clever, just apply an awfully dumb drift
    // compensation with a constant factor, which does what we want. In
    // theory we could calculate the exact drift compensation needed,
    // but it likely would be wrong anyway, and we'd run into the same
    // issues again, except with more complex code.
    // 1 means drifts to positive, -1 means drifts to negative
    double max_drift = vsync / 2;
    double av_diff = mpctx->last_av_difference;
    int new = mpctx->display_sync_drift_dir;
    if (av_diff * -mpctx->display_sync_drift_dir >= 0)
        new = 0;
    if (fabs(av_diff) > max_drift)
        new = av_diff >= 0 ? 1 : -1;

    bool change = mpctx->display_sync_drift_dir != new;
    if (new || change) {
        if (change)
            MP_VERBOSE(mpctx, "Change display sync audio drift: %d\n", new);
        mpctx->display_sync_drift_dir = new;

        double max_correct = opts->sync_max_audio_change / 100;
        double audio_factor = 1 + max_correct * -mpctx->display_sync_drift_dir;

        if (new == 0) {
            // If we're resetting, actually try to be clever and pick a speed
            // which compensates the general drift we're getting.
            double drift = compute_audio_drift(mpctx, vsync);
            if (isnormal(drift)) {
                // other = will be multiplied with audio_factor for final speed
                double other = mpctx->opts->playback_speed * mpctx->speed_factor_v;
                audio_factor = (mpctx->audio_speed - drift) / other;
                MP_VERBOSE(mpctx, "Compensation factor: %f\n", audio_factor);
            }
        }

        audio_factor = MPCLAMP(audio_factor, 1 - max_correct, 1 + max_correct);
        mpctx->speed_factor_a = audio_factor * mpctx->speed_factor_v;
    }
}

// Manipulate frame timing for display sync, or do nothing for normal timing.
static void handle_display_sync_frame(struct MPContext *mpctx,
                                      struct vo_frame *frame)
{
    struct MPOpts *opts = mpctx->opts;
    struct vo *vo = mpctx->video_out;
    int mode = opts->video_sync;

    if (!mpctx->display_sync_active) {
        mpctx->display_sync_error = 0.0;
        mpctx->display_sync_drift_dir = 0;
    }

    mpctx->display_sync_active = false;

    if (!VS_IS_DISP(mode) || mpctx->display_sync_broken)
        return;
    bool resample = mode == VS_DISP_RESAMPLE || mode == VS_DISP_RESAMPLE_VDROP ||
                    mode == VS_DISP_RESAMPLE_NONE;
    bool drop = mode == VS_DISP_VDROP || mode == VS_DISP_RESAMPLE ||
                mode == VS_DISP_ADROP || mode == VS_DISP_RESAMPLE_VDROP;
    drop &= (opts->frame_dropping & 1);

    if (resample && using_spdif_passthrough(mpctx))
        return;

    double vsync = vo_get_vsync_interval(vo) / 1e6;
    if (vsync <= 0)
        return;

    double adjusted_duration = MPMAX(0, mpctx->past_frames[0].approx_duration);
    adjusted_duration /= opts->playback_speed;
    if (adjusted_duration > 0.5)
        return;

    mpctx->speed_factor_v = 1.0;
    if (mode != VS_DISP_VDROP) {
        double best = find_best_speed(mpctx, vsync);
        // If it doesn't work, play at normal speed.
        if (fabs(best - 1.0) <= opts->sync_max_video_change / 100)
            mpctx->speed_factor_v = best;
    }

    double av_diff = mpctx->last_av_difference;
    if (fabs(av_diff) > 0.5) {
        mpctx->display_sync_broken = true;
        return;
    }

    // Determine for how many vsyncs a frame should be displayed. This can be
    // e.g. 2 for 30hz on a 60hz display. It can also be 0 if the video
    // framerate is higher than the display framerate.
    // We use the speed-adjusted (i.e. real) frame duration for this.
    double frame_duration = adjusted_duration / mpctx->speed_factor_v;
    double ratio = (frame_duration + mpctx->display_sync_error) / vsync;
    int num_vsyncs = MPMAX(lrint(ratio), 0);
    double prev_error = mpctx->display_sync_error;
    mpctx->display_sync_error += frame_duration - num_vsyncs * vsync;

    MP_DBG(mpctx, "s=%f vsyncs=%d dur=%f ratio=%f err=%.20f (%f/%f)\n",
           mpctx->speed_factor_v, num_vsyncs, adjusted_duration, ratio,
           mpctx->display_sync_error, mpctx->display_sync_error / vsync,
           mpctx->display_sync_error / frame_duration);

    MP_STATS(mpctx, "value %f avdiff", av_diff);

    // Intended number of additional display frames to drop (<0) or repeat (>0)
    int drop_repeat = 0;

    // If we are too far ahead/behind, attempt to drop/repeat frames.
    // Tolerate some desync to avoid frame dropping due to jitter.
    if (drop && fabs(av_diff) >= 0.020 && fabs(av_diff) / vsync >= 1)
        drop_repeat = -av_diff / vsync; // round towards 0

    // We can only drop all frames at most. We can repeat much more frames,
    // but we still limit it to 10 times the original frames to avoid that
    // corner cases or exceptional situations cause too much havoc.
    drop_repeat = MPCLAMP(drop_repeat, -num_vsyncs, num_vsyncs * 10);
    num_vsyncs += drop_repeat;

    // Estimate the video position, so we can calculate a good A/V difference
    // value below. This is used to estimate A/V drift.
    double time_left = vo_get_delay(vo);

    // We also know that the timing is (necessarily) off, because we have to
    // align frame timings on the vsync boundaries. This is unavoidable, and
    // for the sake of the A/V sync calculations we pretend it's perfect.
    time_left += prev_error;
    // Likewise, we know sync is off, but is going to be compensated.
    time_left += drop_repeat * vsync;

    if (drop_repeat) {
        mpctx->mistimed_frames_total += 1;
        MP_STATS(mpctx, "mistimed");
    }

    mpctx->total_avsync_change = 0;
    update_av_diff(mpctx, time_left * opts->playback_speed);

    mpctx->past_frames[0].num_vsyncs = num_vsyncs;
    mpctx->past_frames[0].av_diff = mpctx->last_av_difference;

    if (resample) {
        adjust_audio_resample_speed(mpctx, vsync);
    } else {
        mpctx->speed_factor_a = 1.0;
    }

    // A bad guess, only needed when reverting to audio sync.
    mpctx->time_frame = time_left;

    frame->vsync_interval = vsync;
    frame->vsync_offset = -prev_error;
    frame->ideal_frame_duration = frame_duration;
    frame->num_vsyncs = num_vsyncs;
    frame->display_synced = true;

    mpctx->display_sync_active = true;
    update_playback_speed(mpctx);

    MP_STATS(mpctx, "value %f aspeed", mpctx->speed_factor_a - 1);
    MP_STATS(mpctx, "value %f vspeed", mpctx->speed_factor_v - 1);
}

static void schedule_frame(struct MPContext *mpctx, struct vo_frame *frame)
{
    handle_display_sync_frame(mpctx, frame);

    if (mpctx->num_past_frames > 1 &&
        ((mpctx->past_frames[1].num_vsyncs >= 0) != mpctx->display_sync_active))
    {
        MP_VERBOSE(mpctx, "Video sync mode %s.\n",
                   mpctx->display_sync_active ? "enabled" : "disabled");
    }

    if (!mpctx->display_sync_active) {
        mpctx->speed_factor_a = 1.0;
        mpctx->speed_factor_v = 1.0;
        update_playback_speed(mpctx);

        update_av_diff(mpctx, mpctx->time_frame > 0 ?
            mpctx->time_frame * mpctx->video_speed : 0);
    }
}

// Determine the mpctx->past_frames[0] frame duration.
static void calculate_frame_duration(struct MPContext *mpctx)
{
    assert(mpctx->num_past_frames >= 1 && mpctx->num_next_frames >= 1);

    double demux_duration = mpctx->vo_chain->container_fps > 0
                            ? 1.0 / mpctx->vo_chain->container_fps : -1;
    double duration = -1;

    if (mpctx->num_next_frames >= 2) {
        double pts0 = mpctx->next_frames[0]->pts;
        double pts1 = mpctx->next_frames[1]->pts;
        if (pts0 != MP_NOPTS_VALUE && pts1 != MP_NOPTS_VALUE && pts1 >= pts0)
            duration = pts1 - pts0;
    } else {
        // E.g. last frame on EOF. Only use it if it's significant.
        if (demux_duration >= 0.1)
            duration = demux_duration;
    }

    // The following code tries to compensate for rounded Matroska timestamps
    // by "unrounding" frame durations, or if not possible, approximating them.
    // These formats usually round on 1ms. (Some muxers do this incorrectly,
    // and might be off by 2ms or more, and compensate for it later by an
    // equal rounding error into the opposite direction. Don't try to deal
    // with them; too much potential damage to timing.)
    double tolerance = 0.0011;

    double total = 0;
    int num_dur = 0;
    for (int n = 1; n < mpctx->num_past_frames; n++) {
        // Eliminate likely outliers using a really dumb heuristic.
        double dur = mpctx->past_frames[n].duration;
        if (dur <= 0 || fabs(dur - duration) >= tolerance)
            break;
        total += dur;
        num_dur += 1;
    }
    double approx_duration = num_dur > 0 ? total / num_dur : duration;

    // Try if the demuxer frame rate fits - if so, just take it.
    if (demux_duration > 0) {
        // Note that even if each timestamp is within rounding tolerance, it
        // could literally not add up (e.g. if demuxer FPS is rounded itself).
        if (fabs(duration - demux_duration) < tolerance &&
            fabs(total - demux_duration * num_dur) < tolerance)
        {
            approx_duration = demux_duration;
        }
    }

    mpctx->past_frames[0].duration = duration;
    mpctx->past_frames[0].approx_duration = approx_duration;
}

void write_video(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;

    if (!mpctx->vo_chain)
        return;
    struct track *track = mpctx->vo_chain->track;
    struct vo *vo = mpctx->vo_chain->vo;

    // Actual playback starts when both audio and video are ready.
    if (mpctx->video_status == STATUS_READY)
        return;

    if (mpctx->paused && mpctx->video_status >= STATUS_READY)
        return;

    int r = video_output_image(mpctx);
    MP_TRACE(mpctx, "video_output_image: %d\n", r);

    if (r < 0)
        goto error;

    if (r == VD_WAIT) // Demuxer will wake us up for more packets to decode.
        return;

    if (r == VD_EOF) {
        int prev_state = mpctx->video_status;
        mpctx->video_status = STATUS_EOF;
        if (mpctx->num_past_frames > 0 && mpctx->past_frames[0].duration > 0) {
            if (vo_still_displaying(vo))
                mpctx->video_status = STATUS_DRAINING;
        }
        mpctx->delay = 0;
        mpctx->last_av_difference = 0;
        MP_DBG(mpctx, "video EOF (status=%d)\n", mpctx->video_status);
        if (prev_state != mpctx->video_status)
            mpctx->sleeptime = 0;
        return;
    }

    if (mpctx->video_status > STATUS_PLAYING)
        mpctx->video_status = STATUS_PLAYING;

    if (r != VD_NEW_FRAME) {
        mpctx->sleeptime = 0; // Decode more in next iteration.
        return;
    }

    // Filter output is different from VO input?
    struct mp_image_params p = mpctx->next_frames[0]->params;
    if (!vo->params || !mp_image_params_equal(&p, vo->params)) {
        // Changing config deletes the current frame; wait until it's finished.
        if (vo_still_displaying(vo))
            return;

        const struct vo_driver *info = mpctx->video_out->driver;
        char extra[20] = {0};
        if (p.p_w != p.p_h) {
            int d_w, d_h;
            mp_image_params_get_dsize(&p, &d_w, &d_h);
            snprintf(extra, sizeof(extra), " => %dx%d", d_w, d_h);
        }
        MP_INFO(mpctx, "VO: [%s] %dx%d%s %s\n",
                info->name, p.w, p.h, extra, vo_format_name(p.imgfmt));
        MP_VERBOSE(mpctx, "VO: Description: %s\n", info->description);

        int vo_r = vo_reconfig(vo, &p);
        if (vo_r < 0) {
            mpctx->error_playing = MPV_ERROR_VO_INIT_FAILED;
            goto error;
        }
        init_vo(mpctx);
    }

    mpctx->time_frame -= get_relative_time(mpctx);
    update_avsync_before_frame(mpctx);

    if (!update_subtitles(mpctx, mpctx->next_frames[0]->pts)) {
        MP_VERBOSE(mpctx, "Video frame delayed due waiting on subtitles.\n");
        return;
    }

    double time_frame = MPMAX(mpctx->time_frame, -1);
    int64_t pts = mp_time_us() + (int64_t)(time_frame * 1e6);

    // wait until VO wakes us up to get more frames
    // (NB: in theory, the 1st frame after display sync mode change uses the
    //      wrong waiting mode)
    if (!vo_is_ready_for_frame(vo, mpctx->display_sync_active ? -1 : pts)) {
        if (video_feed_async_filter(mpctx) < 0)
            goto error;
        return;
    }

    assert(mpctx->num_next_frames >= 1);

    if (mpctx->num_past_frames >= MAX_NUM_VO_PTS)
        mpctx->num_past_frames--;
    MP_TARRAY_INSERT_AT(mpctx, mpctx->past_frames, mpctx->num_past_frames, 0,
                        (struct frame_info){0});
    mpctx->past_frames[0] = (struct frame_info){
        .pts = mpctx->next_frames[0]->pts,
        .num_vsyncs = -1,
    };
    calculate_frame_duration(mpctx);

    struct vo_frame dummy = {
        .pts = pts,
        .duration = -1,
        .still = mpctx->step_frames > 0,
        .num_frames = MPMIN(mpctx->num_next_frames, VO_MAX_REQ_FRAMES),
        .num_vsyncs = 1,
    };
    for (int n = 0; n < dummy.num_frames; n++)
        dummy.frames[n] = mpctx->next_frames[n];
    struct vo_frame *frame = vo_frame_ref(&dummy);

    double diff = mpctx->past_frames[0].approx_duration;
    if (opts->untimed || vo->driver->untimed)
        diff = -1; // disable frame dropping and aspects of frame timing
    if (diff >= 0) {
        // expected A/V sync correction is ignored
        diff /= mpctx->video_speed;
        if (mpctx->time_frame < 0)
            diff += mpctx->time_frame;
        frame->duration = MPCLAMP(diff, 0, 10) * 1e6;
    }

    mpctx->video_pts = mpctx->next_frames[0]->pts;
    mpctx->last_vo_pts = mpctx->video_pts;
    mpctx->playback_pts = mpctx->video_pts;

    shift_frames(mpctx);

    schedule_frame(mpctx, frame);

    mpctx->osd_force_update = true;
    update_osd_msg(mpctx);

    vo_queue_frame(vo, frame);

    // The frames were shifted down; "initialize" the new first entry.
    if (mpctx->num_next_frames >= 1)
        handle_new_frame(mpctx);

    mpctx->shown_vframes++;
    if (mpctx->video_status < STATUS_PLAYING) {
        mpctx->video_status = STATUS_READY;
        // After a seek, make sure to wait until the first frame is visible.
        vo_wait_frame(vo);
        MP_VERBOSE(mpctx, "first video frame after restart shown\n");
    }
    screenshot_flip(mpctx);

    mp_notify(mpctx, MPV_EVENT_TICK, NULL);

    if (mpctx->vo_chain->is_coverart)
        mpctx->video_status = STATUS_EOF;

    if (mpctx->video_status != STATUS_EOF) {
        if (mpctx->step_frames > 0) {
            mpctx->step_frames--;
            if (!mpctx->step_frames && !opts->pause)
                pause_player(mpctx);
        }
        if (mpctx->max_frames == 0 && !mpctx->stop_play)
            mpctx->stop_play = AT_END_OF_FILE;
        if (mpctx->max_frames > 0)
            mpctx->max_frames--;
    }

    mpctx->sleeptime = 0;
    return;

error:
    MP_FATAL(mpctx, "Could not initialize video chain.\n");
    uninit_video_chain(mpctx);
    error_on_track(mpctx, track);
    handle_force_window(mpctx, true);
    mpctx->sleeptime = 0;
}
