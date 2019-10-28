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

#ifndef MPLAYER_SCREENSHOT_H
#define MPLAYER_SCREENSHOT_H

#include <stdbool.h>

struct MPContext;
struct mp_image;
struct mp_log;

// One time initialization at program start.
void screenshot_init(struct MPContext *mpctx);

// Called by the playback core code when a new frame is displayed.
void screenshot_flip(struct MPContext *mpctx);

/* Return the image converted to the given format. If the pixel aspect ratio is
 * not 1:1, the image is scaled as well. Returns NULL on failure.
 */
struct mp_image *convert_image(struct mp_image *image, int destfmt,
                               struct mp_log *log);

// Handlers for the user-facing commands.
void cmd_screenshot(void *p);
void cmd_screenshot_to_file(void *p);
void cmd_screenshot_raw(void *p);

#endif /* MPLAYER_SCREENSHOT_H */
