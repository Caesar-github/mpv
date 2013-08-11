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

#ifndef MPLAYER_COMMAND_H
#define MPLAYER_COMMAND_H

struct MPContext;
struct mp_cmd;

void mp_get_osd_mouse_pos(struct MPContext *mpctx, float *x, float *y);

void run_command(struct MPContext *mpctx, struct mp_cmd *cmd);
char *mp_property_expand_string(struct MPContext *mpctx, char *str);
void property_print_help(void);
int mp_property_do(const char* name, int action, void* val,
                   struct MPContext *mpctx);

#endif /* MPLAYER_COMMAND_H */
