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
#include "options/options.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#include "common/msg.h"

#include "osdep/timer.h"

#include "stream/stream.h"
#include "demux/packet.h"

#include "common/codecs.h"

#include "video/out/vo.h"
#include "video/csputils.h"

#include "demux/stheader.h"
#include "video/decode/vd.h"
#include "video/filter/vf.h"

#include "video/decode/dec_video.h"

extern const vd_functions_t mpcodecs_vd_ffmpeg;

/* Please do not add any new decoders here. If you want to implement a new
 * decoder, add it to libavcodec, except for wrappers around external
 * libraries and decoders requiring binary support. */

const vd_functions_t * const mpcodecs_vd_drivers[] = {
    &mpcodecs_vd_ffmpeg,
    /* Please do not add any new decoders here. If you want to implement a new
     * decoder, add it to libavcodec, except for wrappers around external
     * libraries and decoders requiring binary support. */
    NULL
};

void video_reset_decoding(struct dec_video *d_video)
{
    video_vd_control(d_video, VDCTRL_RESET, NULL);
    if (d_video->vfilter && d_video->vfilter->initialized == 1)
        vf_seek_reset(d_video->vfilter);
    mp_image_unrefp(&d_video->waiting_decoded_mpi);
    d_video->num_buffered_pts = 0;
    d_video->last_pts = MP_NOPTS_VALUE;
    d_video->first_packet_pdts = MP_NOPTS_VALUE;
    d_video->decoded_pts = MP_NOPTS_VALUE;
    d_video->codec_pts = MP_NOPTS_VALUE;
    d_video->codec_dts = MP_NOPTS_VALUE;
}

int video_vd_control(struct dec_video *d_video, int cmd, void *arg)
{
    const struct vd_functions *vd = d_video->vd_driver;
    if (vd)
        return vd->control(d_video, cmd, arg);
    return CONTROL_UNKNOWN;
}

int video_set_colors(struct dec_video *d_video, const char *item, int value)
{
    vf_equalizer_t data;

    data.item = item;
    data.value = value;

    MP_VERBOSE(d_video, "set video colors %s=%d \n", item, value);
    if (d_video->vfilter) {
        int ret = video_vf_vo_control(d_video, VFCTRL_SET_EQUALIZER, &data);
        if (ret == CONTROL_TRUE)
            return 1;
    }
    MP_VERBOSE(d_video, "Video attribute '%s' is not supported by selected vo.\n",
               item);
    return 0;
}

int video_get_colors(struct dec_video *d_video, const char *item, int *value)
{
    vf_equalizer_t data;

    data.item = item;

    MP_VERBOSE(d_video, "get video colors %s \n", item);
    if (d_video->vfilter) {
        int ret = video_vf_vo_control(d_video, VFCTRL_GET_EQUALIZER, &data);
        if (ret == CONTROL_TRUE) {
            *value = data.value;
            return 1;
        }
    }
    return 0;
}

void video_uninit(struct dec_video *d_video)
{
    mp_image_unrefp(&d_video->waiting_decoded_mpi);
    mp_image_unrefp(&d_video->cover_art_mpi);
    if (d_video->vd_driver) {
        MP_VERBOSE(d_video, "Uninit video.\n");
        d_video->vd_driver->uninit(d_video);
    }
    vf_destroy(d_video->vfilter);
    talloc_free(d_video);
}

static int init_video_codec(struct dec_video *d_video, const char *decoder)
{
    if (!d_video->vd_driver->init(d_video, decoder)) {
        MP_VERBOSE(d_video, "Video decoder init failed.\n");
        return 0;
    }
    return 1;
}

struct mp_decoder_list *video_decoder_list(void)
{
    struct mp_decoder_list *list = talloc_zero(NULL, struct mp_decoder_list);
    for (int i = 0; mpcodecs_vd_drivers[i] != NULL; i++)
        mpcodecs_vd_drivers[i]->add_decoders(list);
    return list;
}

static struct mp_decoder_list *mp_select_video_decoders(const char *codec,
                                                        char *selection)
{
    struct mp_decoder_list *list = video_decoder_list();
    struct mp_decoder_list *new = mp_select_decoders(list, codec, selection);
    talloc_free(list);
    return new;
}

static const struct vd_functions *find_driver(const char *name)
{
    for (int i = 0; mpcodecs_vd_drivers[i] != NULL; i++) {
        if (strcmp(mpcodecs_vd_drivers[i]->name, name) == 0)
            return mpcodecs_vd_drivers[i];
    }
    return NULL;
}

bool video_init_best_codec(struct dec_video *d_video, char* video_decoders)
{
    assert(!d_video->vd_driver);
    video_reset_decoding(d_video);
    d_video->has_broken_packet_pts = -10; // needs 10 packets to reach decision

    struct mp_decoder_entry *decoder = NULL;
    struct mp_decoder_list *list =
        mp_select_video_decoders(d_video->header->codec, video_decoders);

    mp_print_decoders(d_video->log, MSGL_V, "Codec list:", list);

    for (int n = 0; n < list->num_entries; n++) {
        struct mp_decoder_entry *sel = &list->entries[n];
        const struct vd_functions *driver = find_driver(sel->family);
        if (!driver)
            continue;
        MP_VERBOSE(d_video, "Opening video decoder %s:%s\n",
                   sel->family, sel->decoder);
        d_video->vd_driver = driver;
        if (init_video_codec(d_video, sel->decoder)) {
            decoder = sel;
            break;
        }
        d_video->vd_driver = NULL;
        MP_WARN(d_video, "Video decoder init failed for "
                "%s:%s\n", sel->family, sel->decoder);
    }

    if (d_video->vd_driver) {
        d_video->decoder_desc =
            talloc_asprintf(d_video, "%s [%s:%s]", decoder->desc, decoder->family,
                            decoder->decoder);
        MP_VERBOSE(d_video, "Selected video codec: %s\n", d_video->decoder_desc);
    } else {
        MP_ERR(d_video, "Failed to initialize a video decoder for codec '%s'.\n",
               d_video->header->codec ? d_video->header->codec : "<unknown>");
    }

    if (d_video->header->missing_timestamps) {
        MP_WARN(d_video, "This stream has no timestamps!\n");
        MP_WARN(d_video, "Making up playback time using %f FPS.\n", d_video->fps);
        MP_WARN(d_video, "Seeking will probably fail badly.\n");
    }

    talloc_free(list);
    return !!d_video->vd_driver;
}

static void add_avi_pts(struct dec_video *d_video, double pts)
{
    if (pts != MP_NOPTS_VALUE) {
        if (d_video->num_buffered_pts == MP_ARRAY_SIZE(d_video->buffered_pts)) {
            MP_ERR(d_video, "Too many buffered pts\n");
        } else {
            for (int i = d_video->num_buffered_pts; i > 0; i--)
                d_video->buffered_pts[i] = d_video->buffered_pts[i - 1];
            d_video->buffered_pts[0] = pts;
            d_video->num_buffered_pts++;
        }
    }
}

static double retrieve_avi_pts(struct dec_video *d_video, double codec_pts)
{
    if (d_video->num_buffered_pts) {
        d_video->num_buffered_pts--;
        return d_video->buffered_pts[d_video->num_buffered_pts];
    }
    MP_ERR(d_video, "No pts value from demuxer to use for frame!\n");
    return MP_NOPTS_VALUE;
}

struct mp_image *video_decode(struct dec_video *d_video,
                              struct demux_packet *packet,
                              int drop_frame)
{
    struct MPOpts *opts = d_video->opts;
    bool avi_pts = d_video->header->video->avi_dts && opts->correct_pts;

    struct demux_packet packet_copy;
    if (packet && packet->dts == MP_NOPTS_VALUE) {
        packet_copy = *packet;
        packet = &packet_copy;
        packet->dts = packet->pts;
    }

    double pkt_pts = packet ? packet->pts : MP_NOPTS_VALUE;
    double pkt_dts = packet ? packet->dts : MP_NOPTS_VALUE;

    double pkt_pdts = pkt_pts == MP_NOPTS_VALUE ? pkt_dts : pkt_pts;
    if (pkt_pdts != MP_NOPTS_VALUE && d_video->first_packet_pdts == MP_NOPTS_VALUE)
        d_video->first_packet_pdts = pkt_pdts;

    if (avi_pts)
        add_avi_pts(d_video, pkt_pdts);

    double prev_codec_pts = d_video->codec_pts;
    double prev_codec_dts = d_video->codec_dts;

    if (d_video->header->video->avi_dts)
        drop_frame = 0;

    MP_STATS(d_video, "start decode video");

    struct mp_image *mpi = d_video->vd_driver->decode(d_video, packet, drop_frame);

    MP_STATS(d_video, "end decode video");

    if (!mpi || drop_frame) {
        talloc_free(mpi);
        return NULL;            // error / skipped frame
    }

    if (opts->field_dominance == 0) {
        mpi->fields |= MP_IMGFIELD_TOP_FIRST | MP_IMGFIELD_INTERLACED;
    } else if (opts->field_dominance == 1) {
        mpi->fields &= ~MP_IMGFIELD_TOP_FIRST;
        mpi->fields |= MP_IMGFIELD_INTERLACED;
    }

    // Note: the PTS is reordered, but the DTS is not. Both should be monotonic.
    double pts = d_video->codec_pts;
    double dts = d_video->codec_dts;

    if (pts == MP_NOPTS_VALUE) {
        d_video->codec_pts = prev_codec_pts;
    } else if (pts < prev_codec_pts) {
        d_video->num_codec_pts_problems++;
    }

    if (dts == MP_NOPTS_VALUE) {
        d_video->codec_dts = prev_codec_dts;
    } else if (dts <= prev_codec_dts) {
        d_video->num_codec_dts_problems++;
    }

    // If PTS is unset, or non-monotonic, fall back to DTS.
    if ((d_video->num_codec_pts_problems > d_video->num_codec_dts_problems ||
         pts == MP_NOPTS_VALUE) && dts != MP_NOPTS_VALUE)
        pts = dts;

    // Alternative PTS determination methods
    if (avi_pts)
        pts = retrieve_avi_pts(d_video, pts);

    if (!opts->correct_pts || pts == MP_NOPTS_VALUE) {
        if (opts->correct_pts && !d_video->header->missing_timestamps)
            MP_WARN(d_video, "No video PTS! Making something up.\n");

        double frame_time = 1.0f / (d_video->fps > 0 ? d_video->fps : 25);
        double base = d_video->first_packet_pdts;
        pts = d_video->decoded_pts;
        if (pts == MP_NOPTS_VALUE) {
            pts = base == MP_NOPTS_VALUE ? 0 : base;
        } else {
            pts += frame_time;
        }
    }

    if (d_video->has_broken_packet_pts < 0)
        d_video->has_broken_packet_pts++;
    if (d_video->num_codec_pts_problems || pkt_pts == MP_NOPTS_VALUE)
        d_video->has_broken_packet_pts = 1;

    mpi->pts = pts;
    d_video->decoded_pts = pts;
    return mpi;
}

int video_reconfig_filters(struct dec_video *d_video,
                           const struct mp_image_params *params)
{
    struct MPOpts *opts = d_video->opts;
    struct mp_image_params p = *params;
    struct sh_video *sh = d_video->header->video;

    // While mp_image_params normally always have to have d_w/d_h set, the
    // decoder signals unknown bitstream aspect ratio with both set to 0.
    float dec_aspect = p.d_w > 0 && p.d_h > 0 ? p.d_w / (float)p.d_h : 0;
    if (d_video->initial_decoder_aspect == 0)
        d_video->initial_decoder_aspect = dec_aspect;

    bool use_container = true;
    switch (opts->aspect_method) {
    case 0:
        // We normally prefer the container aspect, unless the decoder aspect
        // changes at least once.
        if (dec_aspect > 0 && d_video->initial_decoder_aspect != dec_aspect) {
            MP_VERBOSE(d_video, "Using bitstream aspect ratio.\n");
            // Even if the aspect switches back, don't use container aspect again.
            d_video->initial_decoder_aspect = -1;
            use_container = false;
        }
        break;
    case 1:
        use_container = false;
        break;
    }

    if (use_container && sh->aspect > 0) {
        MP_VERBOSE(d_video, "Using container aspect ratio.\n");
        vf_set_dar(&p.d_w, &p.d_h, p.w, p.h, sh->aspect);
    }

    float force_aspect = opts->movie_aspect;
    if (force_aspect >= 0.0) {
        MP_VERBOSE(d_video, "Forcing user-set aspect ratio.\n");
        vf_set_dar(&p.d_w, &p.d_h, p.w, p.h, force_aspect);
    }

    // Assume square pixels if no aspect ratio is set at all.
    if (p.d_w <= 0 || p.d_h <= 0) {
        p.d_w = p.w;
        p.d_h = p.h;
    }

    // Detect colorspace from resolution.
    mp_image_params_guess_csp(&p);

    if (vf_reconfig(d_video->vfilter, params, &p) < 0) {
        MP_FATAL(d_video, "Cannot initialize video filters.\n");
        return -1;
    }

    return 0;
}

// Send a VCTRL, or if it doesn't work, translate it to a VOCTRL and try the VO.
int video_vf_vo_control(struct dec_video *d_video, int vf_cmd, void *data)
{
    if (d_video->vfilter && d_video->vfilter->initialized > 0) {
        int r = vf_control_any(d_video->vfilter, vf_cmd, data);
        if (r != CONTROL_UNKNOWN)
            return r;
    }

    switch (vf_cmd) {
    case VFCTRL_GET_DEINTERLACE:
        return vo_control(d_video->vo, VOCTRL_GET_DEINTERLACE, data) == VO_TRUE;
    case VFCTRL_SET_DEINTERLACE:
        return vo_control(d_video->vo, VOCTRL_SET_DEINTERLACE, data) == VO_TRUE;
    case VFCTRL_SET_EQUALIZER: {
        vf_equalizer_t *eq = data;
        if (!d_video->vo->config_ok)
            return CONTROL_FALSE;                       // vo not configured?
        struct voctrl_set_equalizer_args param = {
            eq->item, eq->value
        };
        return vo_control(d_video->vo, VOCTRL_SET_EQUALIZER, &param) == VO_TRUE;
    }
    case VFCTRL_GET_EQUALIZER: {
        vf_equalizer_t *eq = data;
        if (!d_video->vo->config_ok)
            return CONTROL_FALSE;                       // vo not configured?
        struct voctrl_get_equalizer_args param = {
            eq->item, &eq->value
        };
        return vo_control(d_video->vo, VOCTRL_GET_EQUALIZER, &param) == VO_TRUE;
    }
    }
    return CONTROL_UNKNOWN;
}
