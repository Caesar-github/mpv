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
struct mp_log;

void command_init(struct MPContext *mpctx);
void command_uninit(struct MPContext *mpctx);

int run_command(struct MPContext *mpctx, struct mp_cmd *cmd);
char *mp_property_expand_string(struct MPContext *mpctx, const char *str);
char *mp_property_expand_escaped_string(struct MPContext *mpctx, const char *str);
void property_print_help(struct mp_log *log);
int mp_property_do(const char* name, int action, void* val,
                   struct MPContext *mpctx);

void mp_notify(struct MPContext *mpctx, int event, void *arg);
void mp_notify_property(struct MPContext *mpctx, const char *property);

int mp_get_property_id(const char *name);
uint64_t mp_get_property_event_mask(const char *name);

enum {
    // Must start with the first unused positive value in enum mpv_event_id
    // MPV_EVENT_* and MP_EVENT_* must not overlap.
    INTERNAL_EVENT_BASE = 24,
    MP_EVENT_CACHE_UPDATE,
    MP_EVENT_WIN_RESIZE,
    MP_EVENT_WIN_STATE,
};

bool mp_hook_test_completion(struct MPContext *mpctx, char *type);
void mp_hook_run(struct MPContext *mpctx, char *client, char *type);

#endif /* MPLAYER_COMMAND_H */
