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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>

#include <libavutil/mem.h>

#include "demux/codec_tags.h"

#include "common/codecs.h"
#include "common/msg.h"
#include "common/recorder.h"
#include "misc/bstr.h"
#include "options/options.h"

#include "stream/stream.h"
#include "demux/demux.h"

#include "demux/stheader.h"

#include "dec_audio.h"
#include "ad.h"
#include "audio/format.h"

extern const struct ad_functions ad_lavc;

// Not a real codec - specially treated.
extern const struct ad_functions ad_spdif;

static const struct ad_functions * const ad_drivers[] = {
    &ad_lavc,
    NULL
};

static void uninit_decoder(struct dec_audio *d_audio)
{
    audio_reset_decoding(d_audio);
    if (d_audio->ad_driver) {
        MP_VERBOSE(d_audio, "Uninit audio decoder.\n");
        d_audio->ad_driver->uninit(d_audio);
    }
    d_audio->ad_driver = NULL;
    talloc_free(d_audio->priv);
    d_audio->priv = NULL;
}

static int init_audio_codec(struct dec_audio *d_audio, const char *decoder)
{
    if (!d_audio->ad_driver->init(d_audio, decoder)) {
        MP_VERBOSE(d_audio, "Audio decoder init failed.\n");
        d_audio->ad_driver = NULL;
        uninit_decoder(d_audio);
        return 0;
    }

    return 1;
}

struct mp_decoder_list *audio_decoder_list(void)
{
    struct mp_decoder_list *list = talloc_zero(NULL, struct mp_decoder_list);
    for (int i = 0; ad_drivers[i] != NULL; i++)
        ad_drivers[i]->add_decoders(list);
    return list;
}

static struct mp_decoder_list *audio_select_decoders(struct dec_audio *d_audio)
{
    struct MPOpts *opts = d_audio->opts;
    const char *codec = d_audio->codec->codec;

    struct mp_decoder_list *list = audio_decoder_list();
    struct mp_decoder_list *new =
        mp_select_decoders(d_audio->log, list, codec, opts->audio_decoders);
    if (d_audio->try_spdif && codec) {
        struct mp_decoder_list *spdif =
            select_spdif_codec(codec, opts->audio_spdif);
        mp_append_decoders(spdif, new);
        talloc_free(new);
        new = spdif;
    }
    talloc_free(list);
    return new;
}

static const struct ad_functions *find_driver(const char *name)
{
    for (int i = 0; ad_drivers[i] != NULL; i++) {
        if (strcmp(ad_drivers[i]->name, name) == 0)
            return ad_drivers[i];
    }
    if (strcmp(name, "spdif") == 0)
        return &ad_spdif;
    return NULL;
}

int audio_init_best_codec(struct dec_audio *d_audio)
{
    uninit_decoder(d_audio);
    assert(!d_audio->ad_driver);

    struct mp_decoder_entry *decoder = NULL;
    struct mp_decoder_list *list = audio_select_decoders(d_audio);

    mp_print_decoders(d_audio->log, MSGL_V, "Codec list:", list);

    for (int n = 0; n < list->num_entries; n++) {
        struct mp_decoder_entry *sel = &list->entries[n];
        const struct ad_functions *driver = find_driver(sel->family);
        if (!driver)
            continue;
        MP_VERBOSE(d_audio, "Opening audio decoder %s\n", sel->decoder);
        d_audio->ad_driver = driver;
        if (init_audio_codec(d_audio, sel->decoder)) {
            decoder = sel;
            break;
        }
        MP_WARN(d_audio, "Audio decoder init failed for %s\n", sel->decoder);
    }

    if (d_audio->ad_driver) {
        d_audio->decoder_desc =
            talloc_asprintf(d_audio, "%s (%s)", decoder->decoder, decoder->desc);
        MP_VERBOSE(d_audio, "Selected audio codec: %s\n", d_audio->decoder_desc);
    } else {
        MP_ERR(d_audio, "Failed to initialize an audio decoder for codec '%s'.\n",
               d_audio->codec->codec);
    }

    talloc_free(list);
    return !!d_audio->ad_driver;
}

void audio_uninit(struct dec_audio *d_audio)
{
    if (!d_audio)
        return;
    uninit_decoder(d_audio);
    talloc_free(d_audio);
}

void audio_reset_decoding(struct dec_audio *d_audio)
{
    if (d_audio->ad_driver)
        d_audio->ad_driver->control(d_audio, ADCTRL_RESET, NULL);
    d_audio->pts = MP_NOPTS_VALUE;
    talloc_free(d_audio->current_frame);
    d_audio->current_frame = NULL;
    talloc_free(d_audio->packet);
    d_audio->packet = NULL;
    talloc_free(d_audio->new_segment);
    d_audio->new_segment = NULL;
    d_audio->start = d_audio->end = MP_NOPTS_VALUE;
}

static void fix_audio_pts(struct dec_audio *da)
{
    if (!da->current_frame)
        return;

    double frame_pts = mp_aframe_get_pts(da->current_frame);
    if (frame_pts != MP_NOPTS_VALUE) {
        if (da->pts != MP_NOPTS_VALUE)
            MP_STATS(da, "value %f audio-pts-err", da->pts - frame_pts);

        // Keep the interpolated timestamp if it doesn't deviate more
        // than 1 ms from the real one. (MKV rounded timestamps.)
        if (da->pts == MP_NOPTS_VALUE || fabs(da->pts - frame_pts) > 0.001)
            da->pts = frame_pts;
    }

    if (da->pts == MP_NOPTS_VALUE && da->header->missing_timestamps)
        da->pts = 0;

    mp_aframe_set_pts(da->current_frame, da->pts);

    if (da->pts != MP_NOPTS_VALUE)
        da->pts += mp_aframe_duration(da->current_frame);
}

static bool is_new_segment(struct dec_audio *da, struct demux_packet *p)
{
    return p->segmented &&
        (p->start != da->start || p->end != da->end || p->codec != da->codec);
}

void audio_work(struct dec_audio *da)
{
    if (da->current_frame || !da->ad_driver)
        return;

    if (!da->packet && !da->new_segment &&
        demux_read_packet_async(da->header, &da->packet) == 0)
    {
        da->current_state = DATA_WAIT;
        return;
    }

    if (da->packet && is_new_segment(da, da->packet)) {
        assert(!da->new_segment);
        da->new_segment = da->packet;
        da->packet = NULL;
    }

    if (da->ad_driver->send_packet(da, da->packet)) {
        if (da->recorder_sink)
            mp_recorder_feed_packet(da->recorder_sink, da->packet);

        talloc_free(da->packet);
        da->packet = NULL;
    }

    bool progress = da->ad_driver->receive_frame(da, &da->current_frame);

    da->current_state = da->current_frame ? DATA_OK : DATA_AGAIN;
    if (!progress)
        da->current_state = DATA_EOF;

    fix_audio_pts(da);

    bool segment_end = da->current_state == DATA_EOF;

    if (da->current_frame) {
        mp_aframe_clip_timestamps(da->current_frame, da->start, da->end);
        double frame_pts = mp_aframe_get_pts(da->current_frame);
        if (frame_pts != MP_NOPTS_VALUE && da->start != MP_NOPTS_VALUE)
            segment_end = frame_pts >= da->end;
        if (mp_aframe_get_size(da->current_frame) == 0) {
            talloc_free(da->current_frame);
            da->current_frame = NULL;
        }
    }

    // If there's a new segment, start it as soon as we're drained/finished.
    if (segment_end && da->new_segment) {
        struct demux_packet *new_segment = da->new_segment;
        da->new_segment = NULL;

        if (da->codec == new_segment->codec) {
            audio_reset_decoding(da);
        } else {
            da->codec = new_segment->codec;
            da->ad_driver->uninit(da);
            da->ad_driver = NULL;
            audio_init_best_codec(da);
        }

        da->start = new_segment->start;
        da->end = new_segment->end;

        da->packet = new_segment;
        da->current_state = DATA_AGAIN;
    }
}

// Fetch an audio frame decoded with audio_work(). Returns one of:
//  DATA_OK:    *out_frame is set to a new image
//  DATA_WAIT:  waiting for demuxer; will receive a wakeup signal
//  DATA_EOF:   end of file, no more frames to be expected
//  DATA_AGAIN: dropped frame or something similar
int audio_get_frame(struct dec_audio *da, struct mp_aframe **out_frame)
{
    *out_frame = NULL;
    if (da->current_frame) {
        *out_frame = da->current_frame;
        da->current_frame = NULL;
        return DATA_OK;
    }
    if (da->current_state == DATA_OK)
        return DATA_AGAIN;
    return da->current_state;
}
