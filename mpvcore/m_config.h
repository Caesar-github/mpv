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

#ifndef MPLAYER_M_CONFIG_H
#define MPLAYER_M_CONFIG_H

#include <stddef.h>
#include <stdbool.h>

#include "mpvcore/bstr.h"

// m_config provides an API to manipulate the config variables in MPlayer.
// It makes use of the Options API to provide a context stack that
// allows saving and later restoring the state of all variables.

typedef struct m_profile m_profile_t;
struct m_option;
struct m_option_type;
struct m_sub_options;
struct m_obj_desc;

// Config option
struct m_config_option {
    struct m_config_option *next;
    bool is_generated : 1;
    // Full name (ie option-subopt).
    char *name;
    // Option description.
    const struct m_option *opt;
    // Raw value of the option.
    void *data;
    // If this is a suboption, the option that contains this option.
    struct m_config_option *parent;
};

// Config object
/** \ingroup Config */
typedef struct m_config {
    // Registered options.
    struct m_config_option *opts; // all options, even suboptions

    // List of defined profiles.
    struct m_profile *profiles;
    // Depth when recursively including profiles.
    int profile_depth;

    struct m_opt_backup *backup_opts;

    bool use_profiles;
    int (*includefunc)(struct m_config *conf, char *filename, int flags);

    const void *optstruct_defaults;
    size_t optstruct_size;
    const struct m_option *options; // top-level options
    const char *suboptinit;

    void *optstruct; // struct mpopts or other
} m_config_t;

// Create a new config object.
//  talloc_parent: talloc parent context for the m_config allocation
//  size: size of the optstruct (where option values are stored)
//  defaults: if not NULL, points to a struct of same type as optstruct, which
//            contains default values for all options
//  options: list of options. Each option defines a member of the optstruct
//           and a corresponding option switch or sub-option field.
//  suboptinit: if not NULL, initialize the suboption string (used for presets)
// Note that the m_config object will keep pointers to defaults and options.
struct m_config *m_config_new(void *talloc_parent, size_t size,
                              const void *defaults,
                              const struct m_option *options,
                              const char *suboptinit);

struct m_config *m_config_from_obj_desc(void *talloc_parent,
                                        struct m_obj_desc *desc);

int m_config_set_obj_params(struct m_config *conf, char **args);

// Initialize an object (VO/VF/...) in one go, including legacy handling.
// This is pretty specialized, and is just for convenience.
int m_config_initialize_obj(struct m_config *config, struct m_obj_desc *desc,
                            void **ppriv, char ***pargs);

// Make sure the option is backed up. If it's already backed up, do nothing.
// All backed up options can be restored with m_config_restore_backups().
void m_config_backup_opt(struct m_config *config, const char *opt);

// Call m_config_backup_opt() on all options.
void m_config_backup_all_opts(struct m_config *config);

// Restore all options backed up with m_config_backup_opt(), and delete the
// backups afterwards.
void m_config_restore_backups(struct m_config *config);

enum {
    M_SETOPT_PRE_PARSE_ONLY = 1,    // Silently ignore non-M_OPT_PRE_PARSE opt.
    M_SETOPT_CHECK_ONLY = 2,        // Don't set, just check name/value
    M_SETOPT_FROM_CONFIG_FILE = 4,  // Reject M_OPT_NOCFG opt. (print error)
    M_SETOPT_BACKUP = 8,            // Call m_config_backup_opt() before
};

// Set the named option to the given string.
// flags: combination of M_SETOPT_* flags (0 for normal operation)
// Returns >= 0 on success, otherwise see OptionParserReturn.
int m_config_set_option_ext(struct m_config *config, struct bstr name,
                            struct bstr param, int flags);

/*  Set an option. (Like: m_config_set_option_ext(config, name, param, 0))
 *  \param config The config object.
 *  \param name The option's name.
 *  \param param The value of the option, can be NULL.
 *  \return See \ref OptionParserReturn.
 */
int m_config_set_option(struct m_config *config, struct bstr name,
                        struct bstr param);

static inline int m_config_set_option0(struct m_config *config,
                                       const char *name, const char *param)
{
    return m_config_set_option(config, bstr0(name), bstr0(param));
}

int m_config_parse_suboptions(struct m_config *config, char *name,
                              char *subopts);


/*  Get the option matching the given name.
 *  \param config The config object.
 *  \param name The option's name.
 */
const struct m_option *m_config_get_option(const struct m_config *config,
                                           struct bstr name);

struct m_config_option *m_config_get_co(const struct m_config *config,
                                        struct bstr name);

// Return the n-th option by position. n==0 is the first option. If there are
// less than (n + 1) options, return NULL.
const char *m_config_get_positional_option(const struct m_config *config, int n);

// Return a hint to the option parser whether a parameter is/may be required.
// The option may still accept empty/non-empty parameters independent from
// this, and this function is useful only for handling ambiguous options like
// flags (e.g. "--a" is ok, "--a=yes" is also ok).
// Returns: error code (<0), or number of expected params (0, 1)
int m_config_option_requires_param(struct m_config *config, bstr name);

/*  Print a list of all registered options.
 *  \param config The config object.
 */
void m_config_print_option_list(const struct m_config *config);


/*  Find the profile with the given name.
 *  \param config The config object.
 *  \param arg The profile's name.
 *  \return The profile object or NULL.
 */
struct m_profile *m_config_get_profile0(const struct m_config *config,
                                        char *name);
struct m_profile *m_config_get_profile(const struct m_config *config, bstr name);

/*  Get the profile with the given name, creating it if necessary.
 *  \param config The config object.
 *  \param arg The profile's name.
 *  \return The profile object.
 */
struct m_profile *m_config_add_profile(struct m_config *config, char *name);

/*  Set the description of a profile.
 *  Used by the config file parser when defining a profile.
 *
 *  \param p The profile object.
 *  \param arg The profile's name.
 */
void m_profile_set_desc(struct m_profile *p, bstr desc);

/*  Add an option to a profile.
 *  Used by the config file parser when defining a profile.
 *
 *  \param config The config object.
 *  \param p The profile object.
 *  \param name The option's name.
 *  \param val The option's value.
 */
int m_config_set_profile_option(struct m_config *config, struct m_profile *p,
                                bstr name, bstr val);

/*  Enables profile usage
 *  Used by the config file parser when loading a profile.
 *
 *  \param config The config object.
 *  \param p The profile object.
 *  \param flags M_SETOPT_* bits
 */
void m_config_set_profile(struct m_config *config, struct m_profile *p,
                          int flags);

void *m_config_alloc_struct(void *talloc_parent,
                            const struct m_sub_options *subopts);

#endif /* MPLAYER_M_CONFIG_H */
