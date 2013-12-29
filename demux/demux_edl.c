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

#define HEADER "# mpv EDL v0\n"

// Note: the real work is handled in tl_mpv_edl.c.
static int try_open_file(struct demuxer *demuxer, enum demux_check check)
{
    struct stream *s = demuxer->stream;
    if (s->uncached_type == STREAMTYPE_EDL) {
        demuxer->file_contents = bstr0(s->path);
        return 0;
    }
    if (check >= DEMUX_CHECK_UNSAFE) {
        if (!bstr_equals0(stream_peek(s, strlen(HEADER)), HEADER))
            return -1;
    }
    demuxer->file_contents = stream_read_complete(s, demuxer, 1000000);
    if (demuxer->file_contents.start == NULL)
        return -1;
    bstr_eatstart0(&demuxer->file_contents, HEADER);
    return 0;
}

const struct demuxer_desc demuxer_desc_edl = {
    .name = "edl",
    .desc = "Edit decision list",
    .type = DEMUXER_TYPE_EDL,
    .open = try_open_file,
};
