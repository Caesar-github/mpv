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

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <assert.h>

#include <libavutil/avstring.h>
#include <libavutil/common.h>

#include "osdep/io.h"

#include "input.h"
#include "keycodes.h"
#include "cmd_list.h"
#include "cmd_parse.h"
#include "osdep/timer.h"
#include "common/msg.h"
#include "common/global.h"
#include "options/m_option.h"
#include "options/path.h"
#include "talloc.h"
#include "options/options.h"
#include "bstr/bstr.h"
#include "stream/stream.h"
#include "common/common.h"

#include "joystick.h"

#if HAVE_LIRC
#include "lirc.h"
#endif

#if HAVE_COCOA
#include "osdep/macosx_events.h"
#endif

#define input_lock(ictx)    pthread_mutex_lock(&ictx->mutex)
#define input_unlock(ictx)  pthread_mutex_unlock(&ictx->mutex)

#define MP_MAX_KEY_DOWN 4

struct cmd_bind {
    int keys[MP_MAX_KEY_DOWN];
    int num_keys;
    char *cmd;
    char *location;     // filename/line number of definition
    bool is_builtin;
    struct cmd_bind_section *owner;
};

struct cmd_bind_section {
    struct cmd_bind *binds;
    int num_binds;
    char *section;
    struct mp_rect mouse_area;  // set at runtime, if at all
    bool mouse_area_set;        // mouse_area is valid and should be tested
    struct cmd_bind_section *next;
};

#define MP_MAX_FDS 10

struct input_fd {
    struct mp_log *log;
    int fd;
    int (*read_key)(void *ctx, int fd);
    int (*read_cmd)(void *ctx, int fd, char *dest, int size);
    int (*close_func)(void *ctx, int fd);
    void *ctx;
    unsigned eof : 1;
    unsigned drop : 1;
    unsigned dead : 1;
    unsigned got_cmd : 1;
    unsigned select : 1;
    // These fields are for the cmd fds.
    char *buffer;
    int pos, size;
};

#define MAX_ACTIVE_SECTIONS 5

struct active_section {
    char *name;
    int flags;
};

struct cmd_queue {
    struct mp_cmd *first;
};

struct input_ctx {
    pthread_mutex_t mutex;
    struct mp_log *log;
    struct mpv_global *global;

    bool using_alt_gr;
    bool using_ar;
    bool using_cocoa_media_keys;

    // Autorepeat stuff
    short ar_state;
    int64_t last_ar;

    // Autorepeat config
    unsigned int ar_delay;
    unsigned int ar_rate;
    // Maximum number of queued commands from keypresses (limit to avoid
    // repeated slow commands piling up)
    int key_fifo_size;

    // these are the keys currently down
    int key_down[MP_MAX_KEY_DOWN];
    unsigned int num_key_down;
    int64_t last_key_down;
    bool current_down_cmd_need_release;
    struct mp_cmd *current_down_cmd;

    int doubleclick_time;
    int last_doubleclick_key_down;
    double last_doubleclick_time;

    // Mouse position on the consumer side (as command.c sees it)
    int mouse_x, mouse_y;
    char *mouse_section; // last section to receive mouse event

    // Mouse position on the producer side (as the VO sees it)
    // Unlike mouse_x/y, this can be used to resolve mouse click bindings.
    int mouse_vo_x, mouse_vo_y;

    bool test;

    bool default_bindings;
    // List of command binding sections
    struct cmd_bind_section *cmd_bind_sections;

    // List currently active command sections
    struct active_section active_sections[MAX_ACTIVE_SECTIONS];
    int num_active_sections;

    // Used to track whether we managed to read something while checking
    // events sources. If yes, the sources may have more queued.
    bool got_new_events;

    unsigned int mouse_event_counter;

    struct input_fd fds[MP_MAX_FDS];
    unsigned int num_fds;

    struct cmd_queue cmd_queue;

    bool in_select;
    int wakeup_pipe[2];
};

int async_quit_request;

static int parse_config(struct input_ctx *ictx, bool builtin, bstr data,
                        const char *location, const char *restrict_section);

#define OPT_BASE_STRUCT struct MPOpts

// Our command line options
static const m_option_t input_config[] = {
    OPT_STRING("conf", input.config_file, CONF_GLOBAL),
    OPT_INT("ar-delay", input.ar_delay, CONF_GLOBAL),
    OPT_INT("ar-rate", input.ar_rate, CONF_GLOBAL),
    OPT_PRINT("keylist", mp_print_key_list),
    OPT_PRINT("cmdlist", mp_print_cmd_list),
    OPT_STRING("js-dev", input.js_dev, CONF_GLOBAL),
    OPT_STRING("file", input.in_file, CONF_GLOBAL),
    OPT_FLAG("default-bindings", input.default_bindings, CONF_GLOBAL),
    OPT_FLAG("test", input.test, CONF_GLOBAL),
    { NULL, NULL, 0, 0, 0, 0, NULL}
};

const m_option_t mp_input_opts[] = {
    { "input", (void *)&input_config, CONF_TYPE_SUBCONFIG, 0, 0, 0, NULL},
    OPT_INTRANGE("doubleclick-time", input.doubleclick_time, 0, 0, 1000),
    OPT_FLAG("joystick", input.use_joystick, CONF_GLOBAL),
    OPT_FLAG("lirc", input.use_lirc, CONF_GLOBAL),
    OPT_FLAG("right-alt-gr", input.use_alt_gr, CONF_GLOBAL),
#if HAVE_LIRC
    OPT_STRING("lircconf", input.lirc_configfile, CONF_GLOBAL),
#endif
#if HAVE_COCOA
    OPT_FLAG("ar", input.use_ar, CONF_GLOBAL),
    OPT_FLAG("media-keys", input.use_media_keys, CONF_GLOBAL),
#endif
    { NULL, NULL, 0, 0, 0, 0, NULL}
};

static const char builtin_input_conf[] =
#include "input/input_conf.h"
;

static bool test_rect(struct mp_rect *rc, int x, int y)
{
    return x >= rc->x0 && y >= rc->y0 && x < rc->x1 && y < rc->y1;
}

static int queue_count_cmds(struct cmd_queue *queue)
{
    int res = 0;
    for (struct mp_cmd *cmd = queue->first; cmd; cmd = cmd->queue_next)
        res++;
    return res;
}

static bool queue_has_abort_cmds(struct cmd_queue *queue)
{
    bool ret = false;
    for (struct mp_cmd *cmd = queue->first; cmd; cmd = cmd->queue_next)
        if (mp_input_is_abort_cmd(cmd)) {
            ret = true;
            break;
        }
    return ret;
}

static void queue_remove(struct cmd_queue *queue, struct mp_cmd *cmd)
{
    struct mp_cmd **p_prev = &queue->first;
    while (*p_prev != cmd) {
        p_prev = &(*p_prev)->queue_next;
    }
    // if this fails, cmd was not in the queue
    assert(*p_prev == cmd);
    *p_prev = cmd->queue_next;
}

static void queue_add_head(struct cmd_queue *queue, struct mp_cmd *cmd)
{
    cmd->queue_next = queue->first;
    queue->first = cmd;
}

static void queue_add_tail(struct cmd_queue *queue, struct mp_cmd *cmd)
{
    struct mp_cmd **p_prev = &queue->first;
    while (*p_prev)
        p_prev = &(*p_prev)->queue_next;
    *p_prev = cmd;
    cmd->queue_next = NULL;
}

static struct mp_cmd *queue_peek(struct cmd_queue *queue)
{
    struct mp_cmd *ret = NULL;
    ret = queue->first;
    return ret;
}

static struct mp_cmd *queue_peek_tail(struct cmd_queue *queue)
{
    struct mp_cmd *cur = queue->first;
    while (cur && cur->queue_next)
        cur = cur->queue_next;
    return cur;
}

static bool bind_matches_key(struct cmd_bind *bind, int n, const int *keys);

static void append_bind_info(struct input_ctx *ictx, char **pmsg,
                             struct cmd_bind *bind)
{
    char *msg = *pmsg;
    struct mp_cmd *cmd = mp_input_parse_cmd(ictx, bstr0(bind->cmd),
                                            bind->location);
    bstr stripped = cmd ? cmd->original : bstr0(bind->cmd);
    msg = talloc_asprintf_append(msg, " '%.*s'", BSTR_P(stripped));
    if (!cmd)
        msg = talloc_asprintf_append(msg, " (invalid)");
    if (strcmp(bind->owner->section, "default") != 0)
        msg = talloc_asprintf_append(msg, " in section {%s}",
                                     bind->owner->section);
    msg = talloc_asprintf_append(msg, " in %s", bind->location);
    if (bind->is_builtin)
        msg = talloc_asprintf_append(msg, " (default)");
    talloc_free(cmd);
    *pmsg = msg;
}

static mp_cmd_t *handle_test(struct input_ctx *ictx, int n, int *keys)
{
    char *key_buf = mp_input_get_key_combo_name(keys, n);
    // "$>" to disable property substitution when invoking "show_text"
    char *msg = talloc_asprintf(NULL, "$>Key %s is bound to:\n", key_buf);
    talloc_free(key_buf);

    int count = 0;
    for (struct cmd_bind_section *bs = ictx->cmd_bind_sections;
         bs; bs = bs->next)
    {
        for (int i = 0; i < bs->num_binds; i++) {
            if (bind_matches_key(&bs->binds[i], n, keys)) {
                count++;
                msg = talloc_asprintf_append(msg, "%d. ", count);
                append_bind_info(ictx, &msg, &bs->binds[i]);
                msg = talloc_asprintf_append(msg, "\n");
            }
        }
    }

    if (!count)
        msg = talloc_asprintf_append(msg, "(nothing)");

    MP_VERBOSE(ictx, "%s\n", msg);

    mp_cmd_t *res = mp_input_parse_cmd(ictx, bstr0("show_text \"\""), "");
    res->args[0].v.s = talloc_steal(res, msg);
    return res;
}

static bool bind_matches_key(struct cmd_bind *bind, int num_keys, const int *keys)
{
    if (bind->num_keys != num_keys)
        return false;
    for (int i = 0; i < num_keys; i++) {
        if (bind->keys[i] != keys[i])
            return false;
    }
    return true;
}

static struct cmd_bind_section *get_bind_section(struct input_ctx *ictx,
                                                 bstr section)
{
    struct cmd_bind_section *bind_section = ictx->cmd_bind_sections;

    if (section.len == 0)
        section = bstr0("default");
    while (bind_section) {
        if (bstrcmp0(section, bind_section->section) == 0)
            return bind_section;
        if (bind_section->next == NULL)
            break;
        bind_section = bind_section->next;
    }
    if (bind_section) {
        bind_section->next = talloc_ptrtype(ictx, bind_section->next);
        bind_section = bind_section->next;
    } else {
        ictx->cmd_bind_sections = talloc_ptrtype(ictx, ictx->cmd_bind_sections);
        bind_section = ictx->cmd_bind_sections;
    }
    *bind_section = (struct cmd_bind_section) {
        .section = bstrdup0(bind_section, section),
    };
    return bind_section;
}

static struct cmd_bind *find_bind_for_key_section(struct input_ctx *ictx,
                                                  char *section,
                                                  int num_keys, int *keys)
{
    struct cmd_bind_section *bs = get_bind_section(ictx, bstr0(section));

    if (!num_keys || !bs->num_binds)
        return NULL;

    // Prefer user-defined keys over builtin bindings
    for (int builtin = 0; builtin < 2; builtin++) {
        if (builtin && !ictx->default_bindings)
            break;
        for (int n = 0; n < bs->num_binds; n++) {
            if (bs->binds[n].is_builtin == (bool)builtin &&
                bind_matches_key(&bs->binds[n], num_keys, keys))
                return &bs->binds[n];
        }
    }
    return NULL;
}

static bool any_mouse_buttons_down(int num_keys, int *keys)
{
    for (int n = 0; n < num_keys; n++) {
        if (MP_KEY_IS_MOUSE_BTN_SINGLE(keys[n]))
            return true;
    }
    return false;
}

static bool depends_on_mouse_pos(int num_keys, int *keys)
{
    for (int n = 0; n < num_keys; n++) {
        if (MP_KEY_DEPENDS_ON_MOUSE_POS(keys[n]))
            return true;
    }
    return false;
}

static struct cmd_bind *find_any_bind_for_key(struct input_ctx *ictx,
                                              char *force_section,
                                              int n, int *keys)
{
    if (force_section)
        return find_bind_for_key_section(ictx, force_section, n, keys);

    bool use_mouse = depends_on_mouse_pos(n, keys);
    // Check global state, because MOUSE_MOVE in particular does not include
    // the global state in n/keys.
    bool mouse_down = any_mouse_buttons_down(ictx->num_key_down, ictx->key_down);

    // First look whether a mouse section is capturing all mouse input
    // exclusively (regardless of the active section stack order).
    if (use_mouse && mouse_down) {
        struct cmd_bind *bind =
            find_bind_for_key_section(ictx, ictx->mouse_section, n, keys);
        if (bind)
            return bind;
    }

    for (int i = ictx->num_active_sections - 1; i >= 0; i--) {
        struct active_section *s = &ictx->active_sections[i];
        struct cmd_bind *bind = find_bind_for_key_section(ictx, s->name, n, keys);
        if (bind) {
            struct cmd_bind_section *bs = bind->owner;
            if (!use_mouse || (bs->mouse_area_set && test_rect(&bs->mouse_area,
                                                               ictx->mouse_vo_x,
                                                               ictx->mouse_vo_y)))
                return bind;
        }
        if (s->flags & MP_INPUT_EXCLUSIVE)
            break;
    }

    return NULL;
}

static mp_cmd_t *get_cmd_from_keys(struct input_ctx *ictx, char *force_section,
                                   int n, int *keys)
{
    if (ictx->test)
        return handle_test(ictx, n, keys);

    struct cmd_bind *cmd = find_any_bind_for_key(ictx, force_section, n, keys);
    if (cmd == NULL && n > 1) {
        // Hitting two keys at once, and if there's no binding for this
        // combination, the key hit last should be checked.
        cmd = find_any_bind_for_key(ictx, force_section, 1, (int[]){keys[n - 1]});
    }

    if (cmd == NULL) {
        int msgl = MSGL_WARN;
        if (n == 1 && (keys[0] == MP_KEY_MOUSE_MOVE ||
                       keys[0] == MP_KEY_MOUSE_LEAVE))
            msgl = MSGL_DEBUG;
        char *key_buf = mp_input_get_key_combo_name(keys, n);
        MP_MSG(ictx, msgl, "No bind found for key '%s'.\n", key_buf);
        talloc_free(key_buf);
        return NULL;
    }
    mp_cmd_t *ret = mp_input_parse_cmd(ictx, bstr0(cmd->cmd), cmd->location);
    if (ret) {
        ret->input_section = cmd->owner->section;
        if (mp_msg_test(ictx->log, MSGL_DEBUG)) {
            char *keyname = mp_input_get_key_combo_name(keys, n);
            MP_DBG(ictx, "key '%s' -> '%s' in '%s'\n",
                   keyname, cmd->cmd, ret->input_section);
            talloc_free(keyname);
        }
    } else {
        char *key_buf = mp_input_get_key_combo_name(keys, n);
        MP_ERR(ictx, "Invalid command for bound key '%s': '%s'\n",
               key_buf, cmd->cmd);
        talloc_free(key_buf);
    }
    return ret;
}

static void update_mouse_section(struct input_ctx *ictx)
{
    struct cmd_bind *bind =
        find_any_bind_for_key(ictx, NULL, 1, (int[]){MP_KEY_MOUSE_MOVE});

    char *new_section = bind ? bind->owner->section : "default";

    char *old = ictx->mouse_section;
    ictx->mouse_section = new_section;

    if (strcmp(old, ictx->mouse_section) != 0) {
        MP_DBG(ictx, "input: switch section %s -> %s\n",
               old, ictx->mouse_section);
        struct mp_cmd *cmd =
            get_cmd_from_keys(ictx, old, 1, (int[]){MP_KEY_MOUSE_LEAVE});
        if (cmd)
            queue_add_tail(&ictx->cmd_queue, cmd);
        ictx->got_new_events = true;
    }
}

// Called when the currently held-down key is released. This (usually) sends
// the a key-up versiob of the command associated with the keys that were held
// down.
// If the drop_current parameter is set to true, then don't send the key-up
// command. Unless we've already sent a key-down event, in which case the
// input receiver (the player) must get a key-up event, or it would get stuck
// thinking a key is still held down.
static void release_down_cmd(struct input_ctx *ictx, bool drop_current)
{
    if (ictx->current_down_cmd_need_release)
        drop_current = false;
    if (!drop_current && ictx->current_down_cmd &&
        ictx->current_down_cmd->key_up_follows)
    {
        ictx->current_down_cmd->key_up_follows = false;
        queue_add_tail(&ictx->cmd_queue, ictx->current_down_cmd);
        ictx->got_new_events = true;
    } else {
        talloc_free(ictx->current_down_cmd);
    }
    ictx->current_down_cmd = NULL;
    ictx->current_down_cmd_need_release = false;
    ictx->last_key_down = 0;
    ictx->ar_state = -1;
}

static int find_key_down(struct input_ctx *ictx, int code)
{
    code &= ~(MP_KEY_STATE_UP | MP_KEY_STATE_DOWN | MP_KEY_MODIFIER_MASK);
    for (int j = 0; j < ictx->num_key_down; j++) {
        if ((ictx->key_down[j] & ~MP_KEY_MODIFIER_MASK) == code)
            return j;
    }
    return -1;
}

static void remove_key_down(struct input_ctx *ictx, int code)
{
    int index = find_key_down(ictx, code);
    if (index >= 0) {
        memmove(&ictx->key_down[index], &ictx->key_down[index + 1],
                (ictx->num_key_down - (index + 1)) * sizeof(int));
        ictx->num_key_down -= 1;
    }
}

// Whether a command shall be sent on both key down and key up events.
static bool key_updown_ok(enum mp_command_type cmd)
{
    switch (cmd) {
    case MP_CMD_SCRIPT_DISPATCH:
        return true;
    default:
        return false;
    }
}

// We don't want the append to the command queue indefinitely, because that
// could lead to situations where recovery would take too long. On the other
// hand, don't drop commands that will abort playback.
static bool should_drop_cmd(struct input_ctx *ictx, struct mp_cmd *cmd)
{
    struct cmd_queue *queue = &ictx->cmd_queue;
    return (queue_count_cmds(queue) >= ictx->key_fifo_size &&
            (!mp_input_is_abort_cmd(cmd) || queue_has_abort_cmds(queue)));
}

static void interpret_key(struct input_ctx *ictx, int code, double scale)
{
    /* On normal keyboards shift changes the character code of non-special
     * keys, so don't count the modifier separately for those. In other words
     * we want to have "a" and "A" instead of "a" and "Shift+A"; but a separate
     * shift modifier is still kept for special keys like arrow keys.
     */
    int unmod = code & ~MP_KEY_MODIFIER_MASK;
    if (unmod >= 32 && unmod < MP_KEY_BASE)
        code &= ~MP_KEY_MODIFIER_SHIFT;

    if (mp_msg_test(ictx->log, MSGL_DEBUG)) {
        int noflags = code & ~(MP_KEY_STATE_DOWN | MP_KEY_STATE_UP);
        char *key = mp_input_get_key_name(noflags, NULL);
        MP_DBG(ictx, "key code=%#x '%s'%s%s\n",
               code, key, (code & MP_KEY_STATE_DOWN) ? " down" : "",
               (code & MP_KEY_STATE_UP) ? " up" : "");
        talloc_free(key);
    }

    if (!(code & MP_KEY_STATE_UP) && ictx->num_key_down >= MP_MAX_KEY_DOWN) {
        MP_ERR(ictx, "Too many key down events at the same time\n");
        return;
    }

    if (MP_KEY_DEPENDS_ON_MOUSE_POS(unmod))
        ictx->mouse_event_counter++;
    ictx->got_new_events = true;

    bool key_was_down = find_key_down(ictx, code) >= 0;
    struct mp_cmd *cmd = NULL;

    if (code & MP_KEY_STATE_DOWN) {
        // Check if we don't already have this key as pushed
        if (key_was_down)
            return;
        // Cancel current down-event (there can be only one)
        release_down_cmd(ictx, true);
        ictx->key_down[ictx->num_key_down] = code & ~MP_KEY_STATE_DOWN;
        ictx->num_key_down++;
        update_mouse_section(ictx);
        ictx->last_key_down = mp_time_us();
        ictx->ar_state = 0;
        cmd = get_cmd_from_keys(ictx, NULL, ictx->num_key_down, ictx->key_down);
        if (cmd && should_drop_cmd(ictx, cmd)) {
            ictx->num_key_down--;
            talloc_free(cmd);
            return;
        }
        if (cmd && (code & MP_KEY_EMIT_ON_UP))
            cmd->key_up_follows = true;
        ictx->current_down_cmd = mp_cmd_clone(cmd);
        ictx->current_down_cmd_need_release = false;
    } else if (code & MP_KEY_STATE_UP) {
        if (key_was_down) {
            remove_key_down(ictx, code);
            release_down_cmd(ictx, false);
        }
        update_mouse_section(ictx);
    } else {
        // Press of key with no separate down/up events
        if (key_was_down) {
            // Mixing press events and up/down with the same key is not allowed
            MP_WARN(ictx, "Mixing key presses and up/down.\n");
        }
        update_mouse_section(ictx);
        // Add temporarily (include ongoing down/up events)
        int num_key_down = ictx->num_key_down;
        int key_down[MP_MAX_KEY_DOWN];
        memcpy(key_down, ictx->key_down, num_key_down * sizeof(int));
        // Assume doubleclick events never use down/up, while button events do
        if (MP_KEY_IS_MOUSE_BTN_DBL(code)) {
            // Don't emit "MOUSE_BTN0+MOUSE_BTN0_DBL", just "MOUSE_BTN0_DBL"
            int btn = code - MP_MOUSE_BTN0_DBL + MP_MOUSE_BTN0;
            if (!num_key_down || key_down[num_key_down - 1] != btn)
                return;
            key_down[num_key_down - 1] = code;
        } else {
            key_down[num_key_down] = code;
            num_key_down++;
        }
        cmd = get_cmd_from_keys(ictx, NULL, num_key_down, key_down);
        if (cmd && should_drop_cmd(ictx, cmd)) {
            talloc_free(cmd);
            return;
        }
    }

    if (!cmd)
        return;

    // Don't emit a command on key-down if the key is designed to emit commands
    // on key-up (like mouse buttons). Also, if the command specifically should
    // be sent both on key down and key up, still emit the command.
    if (cmd->key_up_follows && !key_updown_ok(cmd->id)) {
        talloc_free(cmd);
        return;
    }

    cmd->scale = scale;

    if (cmd->key_up_follows)
        ictx->current_down_cmd_need_release = true;
    queue_add_tail(&ictx->cmd_queue, cmd);
}

static void mp_input_feed_key(struct input_ctx *ictx, int code, double scale)
{
    if (code == MP_INPUT_RELEASE_ALL) {
        MP_DBG(ictx, "release all\n");
        ictx->num_key_down = 0;
        release_down_cmd(ictx, false);
        update_mouse_section(ictx);
        return;
    }
    if (code == MP_KEY_MOUSE_LEAVE) {
        update_mouse_section(ictx);
        struct mp_cmd *cmd = get_cmd_from_keys(ictx, NULL, 1, (int[]){code});
        if (cmd)
            queue_add_tail(&ictx->cmd_queue, cmd);
        ictx->got_new_events = true;
        return;
    }
    double now = mp_time_sec();
    int doubleclick_time = ictx->doubleclick_time;
    // ignore system-doubleclick if we generate these events ourselves
    int unmod = code & ~MP_KEY_MODIFIER_MASK;
    if (doubleclick_time && MP_KEY_IS_MOUSE_BTN_DBL(unmod))
        return;
    interpret_key(ictx, code, scale);
    if (code & MP_KEY_STATE_DOWN) {
        code &= ~MP_KEY_STATE_DOWN;
        if (ictx->last_doubleclick_key_down == code
            && now - ictx->last_doubleclick_time < doubleclick_time / 1000.0)
        {
            if (code >= MP_MOUSE_BTN0 && code <= MP_MOUSE_BTN2)
                interpret_key(ictx, code - MP_MOUSE_BTN0 + MP_MOUSE_BTN0_DBL, 1);
        }
        ictx->last_doubleclick_key_down = code;
        ictx->last_doubleclick_time = now;
    }
}

void mp_input_put_key(struct input_ctx *ictx, int code)
{
    input_lock(ictx);
    mp_input_feed_key(ictx, code, 1);
    input_unlock(ictx);
}

void mp_input_put_key_utf8(struct input_ctx *ictx, int mods, struct bstr t)
{
    while (t.len) {
        int code = bstr_decode_utf8(t, &t);
        if (code < 0)
            break;
        mp_input_put_key(ictx, code | mods);
    }
}

void mp_input_put_axis(struct input_ctx *ictx, int direction, double value)
{
    if (value == 0.0)
        return;
    input_lock(ictx);
    mp_input_feed_key(ictx, direction, value);
    input_unlock(ictx);
}

void mp_input_set_mouse_pos(struct input_ctx *ictx, int x, int y)
{
    input_lock(ictx);
    MP_DBG(ictx, "mouse move %d/%d\n", x, y);

    ictx->mouse_event_counter++;
    ictx->mouse_vo_x = x;
    ictx->mouse_vo_y = y;

    update_mouse_section(ictx);
    struct mp_cmd *cmd =
        get_cmd_from_keys(ictx, NULL, 1, (int[]){MP_KEY_MOUSE_MOVE});
    if (!cmd)
        cmd = mp_input_parse_cmd(ictx, bstr0("ignore"), "<internal>");

    if (cmd) {
        cmd->mouse_move = true;
        cmd->mouse_x = x;
        cmd->mouse_y = y;
        if (should_drop_cmd(ictx, cmd)) {
            talloc_free(cmd);
        } else {
            // Coalesce with previous mouse move events (i.e. replace it)
            struct mp_cmd *tail = queue_peek_tail(&ictx->cmd_queue);
            if (tail && tail->mouse_move) {
                queue_remove(&ictx->cmd_queue, tail);
                talloc_free(tail);
            }
            queue_add_tail(&ictx->cmd_queue, cmd);
            ictx->got_new_events = true;
        }
    }
    input_unlock(ictx);
}

unsigned int mp_input_get_mouse_event_counter(struct input_ctx *ictx)
{
    // Make the frontend always display the mouse cursor (as long as it's not
    // forced invisible) if mouse input is desired.
    input_lock(ictx);
    if (mp_input_test_mouse_active(ictx, ictx->mouse_x, ictx->mouse_y))
        ictx->mouse_event_counter++;
    int ret = ictx->mouse_event_counter;
    input_unlock(ictx);
    return ret;
}

int input_default_read_cmd(void *ctx, int fd, char *buf, int l)
{
    while (1) {
        int r = read(fd, buf, l);
        // Error ?
        if (r < 0) {
            if (errno == EINTR)
                continue;
            else if (errno == EAGAIN)
                return MP_INPUT_NOTHING;
            return MP_INPUT_ERROR;
            // EOF ?
        }
        return r;
    }
}

int mp_input_add_fd(struct input_ctx *ictx, int unix_fd, int select,
                    int read_cmd_func(void *ctx, int fd, char *dest, int size),
                    int read_key_func(void *ctx, int fd),
                    int close_func(void *ctx, int fd), void *ctx)
{
    if (select && unix_fd < 0) {
        MP_ERR(ictx, "Invalid fd %d in mp_input_add_fd", unix_fd);
        return 0;
    }

    input_lock(ictx);
    struct input_fd *fd = NULL;
    if (ictx->num_fds == MP_MAX_FDS) {
        MP_ERR(ictx, "Too many file descriptors.\n");
    } else {
        fd = &ictx->fds[ictx->num_fds];
    }
    *fd = (struct input_fd){
        .log = ictx->log,
        .fd = unix_fd,
        .select = select,
        .read_cmd = read_cmd_func,
        .read_key = read_key_func,
        .close_func = close_func,
        .ctx = ctx,
    };
    ictx->num_fds++;
    input_unlock(ictx);
    return !!fd;
}

static void mp_input_rm_fd(struct input_ctx *ictx, int fd)
{
    struct input_fd *fds = ictx->fds;
    unsigned int i;

    for (i = 0; i < ictx->num_fds; i++) {
        if (fds[i].fd == fd)
            break;
    }
    if (i == ictx->num_fds)
        return;
    if (fds[i].close_func)
        fds[i].close_func(fds[i].ctx, fds[i].fd);
    talloc_free(fds[i].buffer);

    if (i + 1 < ictx->num_fds)
        memmove(&fds[i], &fds[i + 1],
                (ictx->num_fds - i - 1) * sizeof(struct input_fd));
    ictx->num_fds--;
}

void mp_input_rm_key_fd(struct input_ctx *ictx, int fd)
{
    input_lock(ictx);
    mp_input_rm_fd(ictx, fd);
    input_unlock(ictx);
}

#define MP_CMD_MAX_SIZE 4096

static int read_cmd(struct input_fd *mp_fd, char **ret)
{
    char *end;
    *ret = NULL;

    // Allocate the buffer if it doesn't exist
    if (!mp_fd->buffer) {
        mp_fd->buffer = talloc_size(NULL, MP_CMD_MAX_SIZE);
        mp_fd->pos = 0;
        mp_fd->size = MP_CMD_MAX_SIZE;
    }

    // Get some data if needed/possible
    while (!mp_fd->got_cmd && !mp_fd->eof && (mp_fd->size - mp_fd->pos > 1)) {
        int r = mp_fd->read_cmd(mp_fd->ctx, mp_fd->fd, mp_fd->buffer + mp_fd->pos,
                                mp_fd->size - 1 - mp_fd->pos);
        // Error ?
        if (r < 0) {
            switch (r) {
            case MP_INPUT_ERROR:
            case MP_INPUT_DEAD:
                MP_ERR(mp_fd, "Error while reading command file descriptor %d: %s\n",
                       mp_fd->fd, strerror(errno));
            case MP_INPUT_NOTHING:
                return r;
            case MP_INPUT_RETRY:
                continue;
            }
            // EOF ?
        } else if (r == 0) {
            mp_fd->eof = 1;
            break;
        }
        mp_fd->pos += r;
        break;
    }

    mp_fd->got_cmd = 0;

    while (1) {
        int l = 0;
        // Find the cmd end
        mp_fd->buffer[mp_fd->pos] = '\0';
        end = strchr(mp_fd->buffer, '\r');
        if (end)
            *end = '\n';
        end = strchr(mp_fd->buffer, '\n');
        // No cmd end ?
        if (!end) {
            // If buffer is full we must drop all until the next \n
            if (mp_fd->size - mp_fd->pos <= 1) {
                MP_ERR(mp_fd, "Command buffer of file descriptor %d is full: "
                       "dropping content.\n", mp_fd->fd);
                mp_fd->pos = 0;
                mp_fd->drop = 1;
            }
            break;
        }
        // We already have a cmd : set the got_cmd flag
        else if ((*ret)) {
            mp_fd->got_cmd = 1;
            break;
        }

        l = end - mp_fd->buffer;

        // Not dropping : put the cmd in ret
        if (!mp_fd->drop)
            *ret = talloc_strndup(NULL, mp_fd->buffer, l);
        else
            mp_fd->drop = 0;
        mp_fd->pos -= l + 1;
        memmove(mp_fd->buffer, end + 1, mp_fd->pos);
    }

    if (*ret)
        return 1;
    else
        return MP_INPUT_NOTHING;
}

static void read_cmd_fd(struct input_ctx *ictx, struct input_fd *cmd_fd)
{
    int r;
    char *text;
    while ((r = read_cmd(cmd_fd, &text)) >= 0) {
        ictx->got_new_events = true;
        struct mp_cmd *cmd = mp_input_parse_cmd(ictx, bstr0(text), "<pipe>");
        talloc_free(text);
        if (cmd)
            queue_add_tail(&ictx->cmd_queue, cmd);
        if (!cmd_fd->got_cmd)
            return;
    }
    if (r == MP_INPUT_ERROR)
        MP_ERR(ictx, "Error on command file descriptor %d\n", cmd_fd->fd);
    else if (r == MP_INPUT_DEAD)
        cmd_fd->dead = true;
}

static void read_key_fd(struct input_ctx *ictx, struct input_fd *key_fd)
{
    int code = key_fd->read_key(key_fd->ctx, key_fd->fd);
    if (code >= 0 || code == MP_INPUT_RELEASE_ALL) {
        mp_input_feed_key(ictx, code, 1);
        return;
    }

    if (code == MP_INPUT_ERROR)
        MP_ERR(ictx, "Error on key input file descriptor %d\n", key_fd->fd);
    else if (code == MP_INPUT_DEAD) {
        MP_ERR(ictx, "Dead key input on file descriptor %d\n", key_fd->fd);
        key_fd->dead = true;
    }
}

static void read_fd(struct input_ctx *ictx, struct input_fd *fd)
{
    if (fd->read_cmd) {
        read_cmd_fd(ictx, fd);
    } else {
        read_key_fd(ictx, fd);
    }
}

static void remove_dead_fds(struct input_ctx *ictx)
{
    for (int i = 0; i < ictx->num_fds; i++) {
        if (ictx->fds[i].dead) {
            mp_input_rm_fd(ictx, ictx->fds[i].fd);
            i--;
        }
    }
}

#if HAVE_POSIX_SELECT

static void input_wait_read(struct input_ctx *ictx, int time)
{
    fd_set fds;
    FD_ZERO(&fds);
    int max_fd = 0;
    for (int i = 0; i < ictx->num_fds; i++) {
        if (!ictx->fds[i].select)
            continue;
        if (ictx->fds[i].fd > max_fd)
            max_fd = ictx->fds[i].fd;
        FD_SET(ictx->fds[i].fd, &fds);
    }
    struct timeval tv, *time_val;
    tv.tv_sec = time / 1000;
    tv.tv_usec = (time % 1000) * 1000;
    time_val = &tv;
    ictx->in_select = true;
    input_unlock(ictx);
    if (select(max_fd + 1, &fds, NULL, NULL, time_val) < 0) {
        if (errno != EINTR)
            MP_ERR(ictx, "Select error: %s\n", strerror(errno));
        FD_ZERO(&fds);
    }
    input_lock(ictx);
    ictx->in_select = false;
    for (int i = 0; i < ictx->num_fds; i++) {
        if (ictx->fds[i].select && !FD_ISSET(ictx->fds[i].fd, &fds))
            continue;
        read_fd(ictx, &ictx->fds[i]);
    }
}

#else

static void input_wait_read(struct input_ctx *ictx, int time)
{
    if (time > 0)
        mp_sleep_us(time * 1000);

    for (int i = 0; i < ictx->num_fds; i++)
        read_fd(ictx, &ictx->fds[i]);
}

#endif

/**
 * \param time time to wait at most for an event in milliseconds
 */
static void read_events(struct input_ctx *ictx, int time)
{
    if (ictx->num_key_down && ictx->ar_rate > 0) {
        time = FFMIN(time, 1000 / ictx->ar_rate);
        time = FFMIN(time, ictx->ar_delay);
    }
    time = FFMAX(time, 0);

    while (1) {
        if (ictx->got_new_events)
            time = 0;
        ictx->got_new_events = false;

        remove_dead_fds(ictx);

        if (time) {
            for (int i = 0; i < ictx->num_fds; i++) {
                if (!ictx->fds[i].select)
                    read_fd(ictx, &ictx->fds[i]);
            }
        }

        if (ictx->got_new_events)
            time = 0;

        input_wait_read(ictx, time);

        // Read until all input FDs are empty
        if (!ictx->got_new_events)
            break;
    }
}

int mp_input_queue_cmd(struct input_ctx *ictx, mp_cmd_t *cmd)
{
    input_lock(ictx);
    ictx->got_new_events = true;
    if (cmd)
        queue_add_tail(&ictx->cmd_queue, cmd);
    input_unlock(ictx);
    mp_input_wakeup(ictx);
    return 1;
}

static mp_cmd_t *check_autorepeat(struct input_ctx *ictx)
{
    // No input : autorepeat ?
    if (ictx->ar_rate > 0 && ictx->ar_state >= 0 && ictx->num_key_down > 0
        && !(ictx->key_down[ictx->num_key_down - 1] & MP_NO_REPEAT_KEY)) {
        int64_t t = mp_time_us();
        if (ictx->last_ar + 2000000 < t)
            ictx->last_ar = t;
        // First time : wait delay
        if (ictx->ar_state == 0
            && (t - ictx->last_key_down) >= ictx->ar_delay * 1000)
        {
            if (!ictx->current_down_cmd) {
                ictx->ar_state = -1;
                return NULL;
            }
            ictx->ar_state = 1;
            ictx->last_ar = ictx->last_key_down + ictx->ar_delay * 1000;
            return mp_cmd_clone(ictx->current_down_cmd);
            // Then send rate / sec event
        } else if (ictx->ar_state == 1
                   && (t - ictx->last_ar) >= 1000000 / ictx->ar_rate) {
            ictx->last_ar += 1000000 / ictx->ar_rate;
            return mp_cmd_clone(ictx->current_down_cmd);
        }
    }
    return NULL;
}

/**
 * \param peek_only when set, the returned command stays in the queue.
 * Do not free the returned cmd whe you set this!
 */
mp_cmd_t *mp_input_get_cmd(struct input_ctx *ictx, int time, int peek_only)
{
    input_lock(ictx);
    if (async_quit_request) {
        struct mp_cmd *cmd = mp_input_parse_cmd(ictx, bstr0("quit"), "");
        queue_add_head(&ictx->cmd_queue, cmd);
    }

    if (ictx->cmd_queue.first)
        time = 0;
    read_events(ictx, time);
    struct cmd_queue *queue = &ictx->cmd_queue;
    if (!queue->first) {
        struct mp_cmd *repeated = check_autorepeat(ictx);
        if (repeated) {
            repeated->repeated = true;
            if (repeated->def && repeated->def->allow_auto_repeat) {
                queue_add_tail(queue, repeated);
            } else {
                talloc_free(repeated);
            }
        }
    }
    struct mp_cmd *ret = queue_peek(queue);
    if (ret && !peek_only) {
        queue_remove(queue, ret);
        if (ret->mouse_move) {
            ictx->mouse_x = ret->mouse_x;
            ictx->mouse_y = ret->mouse_y;
        }
    }
    input_unlock(ictx);
    return ret;
}

void mp_input_get_mouse_pos(struct input_ctx *ictx, int *x, int *y)
{
    input_lock(ictx);
    *x = ictx->mouse_x;
    *y = ictx->mouse_y;
    input_unlock(ictx);
}

// If name is NULL, return "default".
// Return a statically allocated name of the section (i.e. return value never
// gets deallocated).
static char *normalize_section(struct input_ctx *ictx, char *name)
{
    return get_bind_section(ictx, bstr0(name))->section;
}

void mp_input_disable_section(struct input_ctx *ictx, char *name)
{
    input_lock(ictx);
    name = normalize_section(ictx, name);

    // Remove old section, or make sure it's on top if re-enabled
    for (int i = ictx->num_active_sections - 1; i >= 0; i--) {
        struct active_section *as = &ictx->active_sections[i];
        if (strcmp(as->name, name) == 0) {
            MP_TARRAY_REMOVE_AT(ictx->active_sections,
                                ictx->num_active_sections, i);
        }
    }
    input_unlock(ictx);
}

void mp_input_enable_section(struct input_ctx *ictx, char *name, int flags)
{
    input_lock(ictx);
    name = normalize_section(ictx, name);

    mp_input_disable_section(ictx, name);

    MP_VERBOSE(ictx, "enable section '%s'\n", name);

    if (ictx->num_active_sections < MAX_ACTIVE_SECTIONS) {
        ictx->active_sections[ictx->num_active_sections++] =
            (struct active_section) {name, flags};
    }

    MP_DBG(ictx, "active section stack:\n");
    for (int n = 0; n < ictx->num_active_sections; n++) {
        MP_DBG(ictx, " %s %d\n", ictx->active_sections[n].name,
               ictx->active_sections[n].flags);
    }

    input_unlock(ictx);
}

void mp_input_disable_all_sections(struct input_ctx *ictx)
{
    input_lock(ictx);
    ictx->num_active_sections = 0;
    input_unlock(ictx);
}

void mp_input_set_section_mouse_area(struct input_ctx *ictx, char *name,
                                     int x0, int y0, int x1, int y1)
{
    input_lock(ictx);
    struct cmd_bind_section *s = get_bind_section(ictx, bstr0(name));
    s->mouse_area = (struct mp_rect){x0, y0, x1, y1};
    s->mouse_area_set = x0 != x1 && y0 != y1;
    input_unlock(ictx);
}

static bool test_mouse(struct input_ctx *ictx, int x, int y, int rej_flags)
{
    input_lock(ictx);
    bool res = false;
    for (int i = 0; i < ictx->num_active_sections; i++) {
        struct active_section *as = &ictx->active_sections[i];
        if (as->flags & rej_flags)
            continue;
        struct cmd_bind_section *s = get_bind_section(ictx, bstr0(as->name));
        if (s->mouse_area_set && test_rect(&s->mouse_area, x, y)) {
            res = true;
            break;
        }
    }
    input_unlock(ictx);
    return res;
}

bool mp_input_test_mouse_active(struct input_ctx *ictx, int x, int y)
{
    return test_mouse(ictx, x, y, MP_INPUT_ALLOW_HIDE_CURSOR);
}

bool mp_input_test_dragging(struct input_ctx *ictx, int x, int y)
{
    return test_mouse(ictx, x, y, MP_INPUT_ALLOW_VO_DRAGGING);
}

static void bind_dealloc(struct cmd_bind *bind)
{
    talloc_free(bind->cmd);
    talloc_free(bind->location);
}

// builtin: if true, remove all builtin binds, else remove all user binds
static void remove_binds(struct cmd_bind_section *bs, bool builtin)
{
    for (int n = bs->num_binds - 1; n >= 0; n--) {
        if (bs->binds[n].is_builtin == builtin) {
            bind_dealloc(&bs->binds[n]);
            assert(bs->num_binds >= 1);
            bs->binds[n] = bs->binds[bs->num_binds - 1];
            bs->num_binds--;
        }
    }
}

void mp_input_define_section(struct input_ctx *ictx, char *name, char *location,
                             char *contents, bool builtin)
{
    if (!name || !name[0])
        return; // parse_config() changes semantics with restrict_section==empty
    input_lock(ictx);
    if (contents) {
        parse_config(ictx, builtin, bstr0(contents), location, name);
    } else {
        // Disable:
        mp_input_disable_section(ictx, name);
        // Delete:
        struct cmd_bind_section *bs = get_bind_section(ictx, bstr0(name));
        remove_binds(bs, builtin);
    }
    input_unlock(ictx);
}

static void bind_keys(struct input_ctx *ictx, bool builtin, bstr section,
                      const int *keys, int num_keys, bstr command,
                      const char *loc)
{
    struct cmd_bind_section *bs = get_bind_section(ictx, section);
    struct cmd_bind *bind = NULL;

    assert(num_keys <= MP_MAX_KEY_DOWN);

    for (int n = 0; n < bs->num_binds; n++) {
        struct cmd_bind *b = &bs->binds[n];
        if (bind_matches_key(b, num_keys, keys) && b->is_builtin == builtin) {
            bind = b;
            break;
        }
    }

    if (!bind) {
        struct cmd_bind empty = {{0}};
        MP_TARRAY_APPEND(bs, bs->binds, bs->num_binds, empty);
        bind = &bs->binds[bs->num_binds - 1];
    }

    bind_dealloc(bind);

    *bind = (struct cmd_bind) {
        .cmd = bstrdup0(bs->binds, command),
        .location = talloc_strdup(bs->binds, loc),
        .owner = bs,
        .is_builtin = builtin,
        .num_keys = num_keys,
    };
    memcpy(bind->keys, keys, num_keys * sizeof(bind->keys[0]));
}

// restrict_section: every entry is forced to this section name
//                   if NULL, load normally and allow any sections
static int parse_config(struct input_ctx *ictx, bool builtin, bstr data,
                        const char *location, const char *restrict_section)
{
    int n_binds = 0;
    int line_no = 0;
    char *cur_loc = NULL;

    while (data.len) {
        line_no++;
        if (cur_loc)
            talloc_free(cur_loc);
        cur_loc = talloc_asprintf(NULL, "%s:%d", location, line_no);

        bstr line = bstr_strip_linebreaks(bstr_getline(data, &data));
        line = bstr_lstrip(line);
        if (line.len == 0 || bstr_startswith0(line, "#"))
            continue;
        struct bstr command;
        // Find the key name starting a line
        struct bstr keyname = bstr_split(line, WHITESPACE, &command);
        command = bstr_strip(command);
        if (command.len == 0) {
            MP_ERR(ictx, "Unfinished key binding: %.*s at %s\n", BSTR_P(line),
                   cur_loc);
            continue;
        }
        char *name = bstrdup0(NULL, keyname);
        int keys[MP_MAX_KEY_DOWN];
        int num_keys = 0;
        if (!mp_input_get_keys_from_string(name, MP_MAX_KEY_DOWN, &num_keys, keys))
        {
            talloc_free(name);
            MP_ERR(ictx, "Unknown key '%.*s' at %s\n", BSTR_P(keyname), cur_loc);
            continue;
        }
        talloc_free(name);

        bstr section = bstr0(restrict_section);
        if (!section.len) {
            if (bstr_startswith0(command, "{")) {
                int p = bstrchr(command, '}');
                if (p != -1) {
                    section = bstr_strip(bstr_splice(command, 1, p));
                    command = bstr_lstrip(bstr_cut(command, p + 1));
                }
            }
        }

        bind_keys(ictx, builtin, section, keys, num_keys, command, cur_loc);
        n_binds++;

        // Print warnings if invalid commands are encountered.
        talloc_free(mp_input_parse_cmd(ictx, command, cur_loc));
    }

    talloc_free(cur_loc);

    return n_binds;
}

static int parse_config_file(struct input_ctx *ictx, char *file, bool warn)
{
    int r = 0;
    void *tmp = talloc_new(NULL);
    stream_t *s = NULL;

    file = mp_get_user_path(tmp, ictx->global, file);
    if (!mp_path_exists(file)) {
        MP_MSG(ictx, warn ? MSGL_ERR : MSGL_V,
               "Input config file %s not found.\n", file);
        goto done;
    }
    s = stream_open(file, ictx->global);
    if (!s) {
        MP_ERR(ictx, "Can't open input config file %s.\n", file);
        goto done;
    }
    bstr data = stream_read_complete(s, tmp, 1000000);
    if (data.start) {
        MP_VERBOSE(ictx, "Parsing input config file %s\n", file);
        int num = parse_config(ictx, false, data, file, NULL);
        MP_VERBOSE(ictx, "Input config file %s parsed: %d binds\n", file, num);
        r = 1;
    } else {
        MP_ERR(ictx, "Error reading input config file %s\n", file);
    }

done:
    free_stream(s);
    talloc_free(tmp);
    return r;
}

static int close_fd(void *ctx, int fd)
{
    return close(fd);
}

#ifndef __MINGW32__
static int read_wakeup(void *ctx, int fd)
{
    char buf[100];
    read(fd, buf, sizeof(buf));
    return MP_INPUT_NOTHING;
}
#endif

struct input_ctx *mp_input_init(struct mpv_global *global)
{
    struct input_conf *input_conf = &global->opts->input;

    struct input_ctx *ictx = talloc_ptrtype(NULL, ictx);
    *ictx = (struct input_ctx){
        .global = global,
        .log = mp_log_new(ictx, global->log, "input"),
        .key_fifo_size = input_conf->key_fifo_size,
        .doubleclick_time = input_conf->doubleclick_time,
        .ar_state = -1,
        .ar_delay = input_conf->ar_delay,
        .ar_rate = input_conf->ar_rate,
        .default_bindings = input_conf->default_bindings,
        .mouse_section = "default",
        .test = input_conf->test,
        .wakeup_pipe = {-1, -1},
    };

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&ictx->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    // Setup default section, so that it does nothing.
    mp_input_enable_section(ictx, NULL, MP_INPUT_ALLOW_VO_DRAGGING |
                                        MP_INPUT_ALLOW_HIDE_CURSOR);
    mp_input_set_section_mouse_area(ictx, NULL, INT_MIN, INT_MIN, INT_MAX, INT_MAX);

    // "Uncomment" the default key bindings in etc/input.conf and add them.
    // All lines that do not start with '# ' are parsed.
    bstr builtin = bstr0(builtin_input_conf);
    while (builtin.len) {
        bstr line = bstr_getline(builtin, &builtin);
        bstr_eatstart0(&line, "#");
        if (!bstr_startswith0(line, " "))
            parse_config(ictx, true, line, "<builtin>", NULL);
    }

#ifndef __MINGW32__
    int ret = pipe(ictx->wakeup_pipe);
    if (ret == 0) {
        for (int i = 0; i < 2 && ret >= 0; i++) {
            mp_set_cloexec(ictx->wakeup_pipe[i]);
            ret = fcntl(ictx->wakeup_pipe[i], F_GETFL);
            if (ret < 0)
                break;
            ret = fcntl(ictx->wakeup_pipe[i], F_SETFL, ret | O_NONBLOCK);
            if (ret < 0)
                break;
        }
    }
    if (ret < 0)
        MP_ERR(ictx, "Failed to initialize wakeup pipe: %s\n", strerror(errno));
    else
        mp_input_add_fd(ictx, ictx->wakeup_pipe[0], true, NULL, read_wakeup,
                        NULL, NULL);
#endif

    bool config_ok = false;
    if (input_conf->config_file)
        config_ok = parse_config_file(ictx, input_conf->config_file, true);
    if (!config_ok && global->opts->load_config) {
        // Try global conf dir
        char *file = mp_find_config_file(NULL, global, "input.conf");
        config_ok = file && parse_config_file(ictx, file, false);
        talloc_free(file);
    }
    if (!config_ok) {
        MP_VERBOSE(ictx, "Falling back on default (hardcoded) input config\n");
    }

#if HAVE_JOYSTICK
    if (input_conf->use_joystick)
        mp_input_joystick_init(ictx, ictx->log, input_conf->js_dev);
#endif

#if HAVE_LIRC
    if (input_conf->use_lirc)
        mp_input_lirc_init(ictx, ictx->log, input_conf->lirc_configfile);
#endif

    if (input_conf->use_alt_gr) {
        ictx->using_alt_gr = true;
    }

#if HAVE_COCOA
    if (input_conf->use_ar) {
        cocoa_init_apple_remote();
        ictx->using_ar = true;
    }

    if (input_conf->use_media_keys) {
        cocoa_init_media_keys();
        ictx->using_cocoa_media_keys = true;
    }
#endif

    if (input_conf->in_file) {
        int mode = O_RDONLY;
#ifndef __MINGW32__
        // Use RDWR for FIFOs to ensure they stay open over multiple accesses.
        // Note that on Windows due to how the API works, using RDONLY should
        // be ok.
        struct stat st;
        if (stat(input_conf->in_file, &st) == 0 && S_ISFIFO(st.st_mode))
            mode = O_RDWR;
        mode |= O_NONBLOCK;
#endif
        int in_file_fd = open(input_conf->in_file, mode);
        if (in_file_fd >= 0)
            mp_input_add_fd(ictx, in_file_fd, 1, input_default_read_cmd, NULL, close_fd, NULL);
        else
            MP_ERR(ictx, "Can't open %s: %s\n", input_conf->in_file,
                   strerror(errno));
    }

    return ictx;
}

static void clear_queue(struct cmd_queue *queue)
{
    while (queue->first) {
        struct mp_cmd *item = queue->first;
        queue_remove(queue, item);
        talloc_free(item);
    }
}

void mp_input_uninit(struct input_ctx *ictx)
{
    if (!ictx)
        return;

#if HAVE_COCOA
    if (ictx->using_ar) {
        cocoa_uninit_apple_remote();
    }

    if (ictx->using_cocoa_media_keys) {
        cocoa_uninit_media_keys();
    }
#endif

    for (int i = 0; i < ictx->num_fds; i++) {
        if (ictx->fds[i].close_func)
            ictx->fds[i].close_func(ictx->fds[i].ctx, ictx->fds[i].fd);
    }
    for (int i = 0; i < 2; i++) {
        if (ictx->wakeup_pipe[i] != -1)
            close(ictx->wakeup_pipe[i]);
    }
    clear_queue(&ictx->cmd_queue);
    talloc_free(ictx->current_down_cmd);
    pthread_mutex_destroy(&ictx->mutex);
    talloc_free(ictx);
}

void mp_input_wakeup(struct input_ctx *ictx)
{
    input_lock(ictx);
    bool send_wakeup = ictx->in_select;
    ictx->got_new_events = true;
    input_unlock(ictx);
    // Safe without locking
    if (send_wakeup && ictx->wakeup_pipe[1] >= 0)
        write(ictx->wakeup_pipe[1], &(char){0}, 1);
}

static bool test_abort(struct input_ctx *ictx)
{
    if (async_quit_request || queue_has_abort_cmds(&ictx->cmd_queue)) {
        MP_WARN(ictx, "Received command to move to another file. "
                "Aborting current processing.\n");
        return true;
    }
    return false;
}

/**
 * \param time time to wait for an interruption in milliseconds
 */
int mp_input_check_interrupt(struct input_ctx *ictx, int time)
{
    input_lock(ictx);
    bool res = test_abort(ictx);
    if (!res) {
        read_events(ictx, time);
        res = test_abort(ictx);
    }
    input_unlock(ictx);
    return res;
}

bool mp_input_use_alt_gr(struct input_ctx *ictx)
{
    return ictx->using_alt_gr;
}

struct mp_cmd *mp_input_parse_cmd(struct input_ctx *ictx, bstr str,
                                  const char *location)
{
    return mp_input_parse_cmd_(ictx->log, str, location);
}
