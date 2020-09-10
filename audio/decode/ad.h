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

#ifndef MPLAYER_AD_H
#define MPLAYER_AD_H

#include "common/codecs.h"
#include "demux/stheader.h"
#include "demux/demux.h"

#include "audio/format.h"
#include "audio/aframe.h"
#include "dec_audio.h"

struct mp_decoder_list;

/* interface of audio decoder drivers */
struct ad_functions {
    const char *name;
    void (*add_decoders)(struct mp_decoder_list *list);
    int (*init)(struct dec_audio *da, const char *decoder);
    void (*uninit)(struct dec_audio *da);
    int (*control)(struct dec_audio *da, int cmd, void *arg);
    // Return whether or not the packet has been consumed.
    bool (*send_packet)(struct dec_audio *da, struct demux_packet *pkt);
    // Return whether decoding is still going on (false if EOF was reached).
    // Never returns false & *out set, but can return true with !*out.
    bool (*receive_frame)(struct dec_audio *da, struct mp_aframe **out);
};

enum ad_ctrl {
    ADCTRL_RESET = 1,   // flush and reset state, e.g. after seeking
};

#endif /* MPLAYER_AD_H */
