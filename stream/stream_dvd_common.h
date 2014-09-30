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

#ifndef MPLAYER_STREAM_DVD_COMMON_H
#define MPLAYER_STREAM_DVD_COMMON_H

#include <inttypes.h>
#include <stdbool.h>
#include "stream.h"

extern const char * const dvd_audio_stream_channels[6];
extern const char * const dvd_audio_stream_types[8];

void dvd_set_speed(stream_t *stream, char *device, unsigned speed);
int mp_dvdtimetomsec(dvd_time_t *dt);

int dvd_probe(const char *path, const char *ext, const char *sig);

#endif /* MPLAYER_STREAM_DVD_COMMON_H */
