/*
 * Get path to config dir/file.
 *
 * Return Values:
 *   Returns the pointer to the ALLOCATED buffer containing the
 *   zero terminated path string. This buffer has to be FREED
 *   by the caller.
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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "config.h"

#include "common/global.h"
#include "common/msg.h"
#include "options/options.h"
#include "options/path.h"
#include "talloc.h"
#include "osdep/io.h"
#include "osdep/path.h"

typedef char *(*lookup_fun)(void *tctx, struct mpv_global *global, const char *);
static const lookup_fun config_lookup_functions[] = {
    mp_find_user_config_file,
#if HAVE_COCOA
    mp_get_macosx_bundled_path,
#endif
    mp_find_global_config_file,
    NULL
};

#define STRNULL(s) ((s) ? (s) : "(NULL)")

char *mp_find_config_file(void *talloc_ctx, struct mpv_global *global,
                          const char *filename)
{
    struct MPOpts *opts = global->opts;

    char *res = NULL;
    if (opts->load_config) {
        if (opts->force_configdir && opts->force_configdir[0]) {
            // Always force the local config dir.
            res = mp_find_user_config_file(talloc_ctx, global, filename);
        } else {
            for (int i = 0; config_lookup_functions[i] != NULL; i++) {
                res = config_lookup_functions[i](talloc_ctx, global, filename);
                if (!res)
                    continue;

                if (mp_path_exists(res))
                    break;

                talloc_free(res);
                res = NULL;
            }
        }
    }
    MP_VERBOSE(global, "any config path: '%s' -> '%s'\n", STRNULL(filename),
               STRNULL(res));
    return res;
}

char *mp_find_user_config_file(void *talloc_ctx, struct mpv_global *global,
                               const char *filename)
{
    struct MPOpts *opts = global->opts;

    char *res = NULL;
    if (opts->load_config) {
        if (opts->force_configdir && opts->force_configdir[0]) {
            res = mp_path_join(talloc_ctx, bstr0(opts->force_configdir),
                               bstr0(filename));
        } else {
            char *homedir = getenv("MPV_HOME");
            char *configdir = NULL;

            if (!homedir) {
#ifdef _WIN32
                res = talloc_steal(talloc_ctx, mp_get_win_config_path(filename));
#endif
                homedir = getenv("HOME");
                configdir = ".mpv";
            }

            if (!res && homedir) {
                char *temp = mp_path_join(NULL, bstr0(homedir), bstr0(configdir));
                res = mp_path_join(talloc_ctx, bstr0(temp), bstr0(filename));
                talloc_free(temp);
            }
        }
    }

    MP_VERBOSE(global, "user config path: '%s' -> '%s'\n", STRNULL(filename),
               STRNULL(res));
    return res;
}

char *mp_find_global_config_file(void *talloc_ctx, struct mpv_global *global,
                                 const char *filename)
{
    struct MPOpts *opts = global->opts;
    char *res = NULL;

    if (opts->load_config && !(opts->force_configdir && opts->force_configdir[0]))
    {
        if (filename) {
            res = mp_path_join(talloc_ctx, bstr0(MPLAYER_CONFDIR), bstr0(filename));
        } else {
            res = talloc_strdup(talloc_ctx, MPLAYER_CONFDIR);
        }
    }

    MP_VERBOSE(global, "global config path: '%s' -> '%s'\n", STRNULL(filename),
               STRNULL(res));
    return res;
}

char *mp_get_user_path(void *talloc_ctx, struct mpv_global *global,
                       const char *path)
{
    if (!path)
        return NULL;
    char *res = NULL;
    bstr bpath = bstr0(path);
    if (bstr_eatstart0(&bpath, "~")) {
        // parse to "~" <prefix> "/" <rest>
        bstr prefix, rest;
        if (bstr_split_tok(bpath, "/", &prefix, &rest)) {
            const char *rest0 = rest.start; // ok in this case
            if (bstr_equals0(prefix, "~")) {
                res = mp_find_user_config_file(talloc_ctx, global, rest0);
            } else if (bstr_equals0(prefix, "")) {
                res = mp_path_join(talloc_ctx, bstr0(getenv("HOME")), rest);
            }
        }
    }
    if (!res)
        res = talloc_strdup(talloc_ctx, path);
    MP_VERBOSE(global, "user path: '%s' -> '%s'\n", path, res);
    return res;
}

char *mp_basename(const char *path)
{
    char *s;

#if HAVE_DOS_PATHS
    s = strrchr(path, '\\');
    if (s)
        path = s + 1;
    s = strrchr(path, ':');
    if (s)
        path = s + 1;
#endif
    s = strrchr(path, '/');
    return s ? s + 1 : (char *)path;
}

struct bstr mp_dirname(const char *path)
{
    struct bstr ret = {
        (uint8_t *)path, mp_basename(path) - path
    };
    if (ret.len == 0)
        return bstr0(".");
    return ret;
}

char *mp_splitext(const char *path, bstr *root)
{
    assert(path);
    const char *split = strrchr(path, '.');
    if (!split || !split[1] || strchr(split, '/'))
        return NULL;
    if (root)
        *root = (bstr){(char *)path, split - path};
    return (char *)split + 1;
}

char *mp_path_join(void *talloc_ctx, struct bstr p1, struct bstr p2)
{
    if (p1.len == 0)
        return bstrdup0(talloc_ctx, p2);
    if (p2.len == 0)
        return bstrdup0(talloc_ctx, p1);

#if HAVE_DOS_PATHS
    if (p2.len >= 2 && p2.start[1] == ':'
        || p2.start[0] == '\\' || p2.start[0] == '/')
#else
    if (p2.start[0] == '/')
#endif
        return bstrdup0(talloc_ctx, p2);   // absolute path

    bool have_separator;
    int endchar1 = p1.start[p1.len - 1];
#if HAVE_DOS_PATHS
    have_separator = endchar1 == '/' || endchar1 == '\\'
                     || p1.len == 2 && endchar1 == ':'; // "X:" only
#else
    have_separator = endchar1 == '/';
#endif

    return talloc_asprintf(talloc_ctx, "%.*s%s%.*s", BSTR_P(p1),
                           have_separator ? "" : "/", BSTR_P(p2));
}

char *mp_getcwd(void *talloc_ctx)
{
    char *wd = talloc_array(talloc_ctx, char, 20);
    while (getcwd(wd, talloc_get_size(wd)) == NULL) {
        if (errno != ERANGE) {
            talloc_free(wd);
            return NULL;
        }
        wd = talloc_realloc(talloc_ctx, wd, char, talloc_get_size(wd) * 2);
    }
    return wd;
}

bool mp_path_exists(const char *path)
{
    struct stat st;
    return mp_stat(path, &st) == 0;
}

bool mp_path_isdir(const char *path)
{
    struct stat st;
    return mp_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Return false if it's considered a normal local filesystem path.
bool mp_is_url(bstr path)
{
    int proto = bstr_find0(path, "://");
    if (proto < 1)
        return false;
    // The protocol part must be alphanumeric, otherwise it's not an URL.
    for (int i = 0; i < proto; i++) {
        unsigned char c = path.start[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '_')
            return false;
    }
    return true;
}

// Return the protocol part of path, e.g. "http" if path is "http://...".
// On success, out_url (if not NULL) is set to the part after the "://".
bstr mp_split_proto(bstr path, bstr *out_url)
{
    if (!mp_is_url(path))
        return (bstr){0};
    bstr r;
    bstr_split_tok(path, "://", &r, out_url ? out_url : &(bstr){0});
    return r;
}

void mp_mk_config_dir(struct mpv_global *global, char *subdir)
{
    void *tmp = talloc_new(NULL);
    char *confdir = mp_find_user_config_file(tmp, global, "");
    if (confdir) {
        if (subdir)
            confdir = mp_path_join(tmp, bstr0(confdir), bstr0(subdir));
        mkdir(confdir, 0777);
    }
    talloc_free(tmp);
}
