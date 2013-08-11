/*
 * This file is part of MPlayer.
 *
 * Original author: Uoti Urpala
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

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "demux.h"
#include "stream/stream.h"

static int try_open_file(struct demuxer *demuxer, enum demux_check check)
{
    struct stream *s = demuxer->stream;
    if (check >= DEMUX_CHECK_UNSAFE) {
        const char header[] = "mplayer EDL file";
        const int len = sizeof(header) - 1;
        char buf[len];
        if (stream_read(s, buf, len) < len)
            return -1;
        if (strncmp(buf, header, len))
            return -1;
        stream_seek(s, 0);
    }
    demuxer->file_contents = stream_read_complete(s, demuxer, 1000000);
    if (demuxer->file_contents.start == NULL)
        return -1;
    return 0;
}

const struct demuxer_desc demuxer_desc_edl = {
    .name = "edl",
    .desc = "mplayer2 edit decision list",
    .type = DEMUXER_TYPE_EDL,
    .open = try_open_file,
};
