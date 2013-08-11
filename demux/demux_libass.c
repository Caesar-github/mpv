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

// Note: just wraps libass, and makes the subtitle track available though
//       sh_sub->track. It doesn't produce packets and doesn't support seeking.

#include <ass/ass.h>
#include <ass/ass_types.h>

#include "mpvcore/options.h"
#include "mpvcore/mp_msg.h"
#include "mpvcore/charset_conv.h"
#include "stream/stream.h"
#include "demux.h"

#define PROBE_SIZE (8 * 1024)

struct priv {
    ASS_Track *track;
};

static int d_check_file(struct demuxer *demuxer, enum demux_check check)
{
    const char *user_cp = demuxer->opts->sub_cp;
    struct stream *s = demuxer->stream;
    // Older versions of libass will behave strange if renderer and track
    // library handles mismatch, so make sure everything uses a global handle.
    ASS_Library *lib = demuxer->params ? demuxer->params->ass_library : NULL;
    if (!lib)
        return -1;

    if (check >= DEMUX_CHECK_UNSAFE) {
        // Probe by loading a part of the beginning of the file with libass.
        // Incomplete scripts are usually ok, and we hope libass is not verbose
        // when dealing with (from its perspective) completely broken binary
        // garbage.

        bstr buf = stream_peek(s, PROBE_SIZE);
        // Older versions of libass will overwrite the input buffer, and despite
        // passing length, expect a 0 termination.
        void *tmp = talloc_size(NULL, buf.len + 1);
        memcpy(tmp, buf.start, buf.len);
        buf.start = tmp;
        buf.start[buf.len] = '\0';
        bstr cbuf = mp_charset_guess_and_conv_to_utf8(buf, user_cp,
                                                      MP_ICONV_ALLOW_CUTOFF);
        if (cbuf.start == NULL)
            cbuf = buf;
        ASS_Track *track = ass_read_memory(lib, cbuf.start, cbuf.len, NULL);
        if (cbuf.start != buf.start)
            talloc_free(cbuf.start);
        talloc_free(buf.start);
        if (!track)
            return -1;
        ass_free_track(track);
    }

    // Actually load the full thing.

    bstr buf = stream_read_complete(s, NULL, 100000000);
    if (!buf.start) {
        mp_tmsg(MSGT_ASS, MSGL_ERR, "Refusing to load subtitle file "
                "larger than 100 MB: %s\n", demuxer->filename);
        return -1;
    }
    bstr cbuf = mp_charset_guess_and_conv_to_utf8(buf, user_cp,
                                                  MP_ICONV_VERBOSE);
    if (cbuf.start == NULL)
        cbuf = buf;
    ASS_Track *track = ass_read_memory(lib, cbuf.start, cbuf.len, NULL);
    if (cbuf.start != buf.start)
        talloc_free(cbuf.start);
    talloc_free(buf.start);
    if (!track)
        return -1;

    track->name = strdup(demuxer->filename);

    struct priv *p = talloc_ptrtype(demuxer, p);
    *p = (struct priv) {
        .track = track,
    };
    demuxer->priv = p;

    struct sh_stream *sh = new_sh_stream(demuxer, STREAM_SUB);
    sh->sub->track = track;
    sh->codec = "ass";

    return 0;
}

static void d_close(struct demuxer *demuxer)
{
    struct priv *p = demuxer->priv;
    if (p) {
        if (p->track)
            ass_free_track(p->track);
    }
}

const struct demuxer_desc demuxer_desc_libass = {
    .name = "libass",
    .desc = "ASS/SSA subtitles (libass)",
    .open = d_check_file,
    .close = d_close,
};
