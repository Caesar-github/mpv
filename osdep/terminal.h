/*
 * GyS-TermIO v2.0 (for GySmail v3)
 * a very small replacement of ncurses library
 *
 * copyright (C) 1999 A'rpi/ESP-team
 *
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

#ifndef MPLAYER_GETCH2_H
#define MPLAYER_GETCH2_H

#include <stdbool.h>
#include <stdio.h>

struct input_ctx;

/* Global initialization for terminal output. */
int terminal_init(void);

/* Setup ictx to read keys from the terminal */
void terminal_setup_getch(struct input_ctx *ictx);

/* Undo terminal_init(), and also terminal_setup_getch() */
void terminal_uninit(void);

/* Return whether the process has been backgrounded. */
bool terminal_in_background(void);

/* Get terminal-size in columns/rows. */
void terminal_get_size(int *w, int *h);

// Windows only.
void mp_write_console_ansi(void *wstream, char *buf);

#endif /* MPLAYER_GETCH2_H */
