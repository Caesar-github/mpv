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

#include <ctype.h>

#include "user_shaders.h"

static bool parse_rpn_szexpr(struct bstr line, struct szexp out[MAX_SZEXP_SIZE])
{
    int pos = 0;

    while (line.len > 0) {
        struct bstr word = bstr_strip(bstr_splitchar(line, &line, ' '));
        if (word.len == 0)
            continue;

        if (pos >= MAX_SZEXP_SIZE)
            return false;

        struct szexp *exp = &out[pos++];

        if (bstr_eatend0(&word, ".w") || bstr_eatend0(&word, ".width")) {
            exp->tag = SZEXP_VAR_W;
            exp->val.varname = word;
            continue;
        }

        if (bstr_eatend0(&word, ".h") || bstr_eatend0(&word, ".height")) {
            exp->tag = SZEXP_VAR_H;
            exp->val.varname = word;
            continue;
        }

        switch (word.start[0]) {
        case '+': exp->tag = SZEXP_OP2; exp->val.op = SZEXP_OP_ADD; continue;
        case '-': exp->tag = SZEXP_OP2; exp->val.op = SZEXP_OP_SUB; continue;
        case '*': exp->tag = SZEXP_OP2; exp->val.op = SZEXP_OP_MUL; continue;
        case '/': exp->tag = SZEXP_OP2; exp->val.op = SZEXP_OP_DIV; continue;
        case '!': exp->tag = SZEXP_OP1; exp->val.op = SZEXP_OP_NOT; continue;
        case '>': exp->tag = SZEXP_OP2; exp->val.op = SZEXP_OP_GT;  continue;
        case '<': exp->tag = SZEXP_OP2; exp->val.op = SZEXP_OP_LT;  continue;
        }

        if (isdigit(word.start[0])) {
            exp->tag = SZEXP_CONST;
            if (bstr_sscanf(word, "%f", &exp->val.cval) != 1)
                return false;
            continue;
        }

        // Some sort of illegal expression
        return false;
    }

    return true;
}

// Returns false if no more shaders could be parsed
bool parse_user_shader_pass(struct mp_log *log, struct bstr *body,
                            struct gl_user_shader *out)
{
    if (!body || !out || !body->start || body->len == 0)
        return false;

    *out = (struct gl_user_shader){
        .offset = identity_trans,
        .width = {{ SZEXP_VAR_W, { .varname = bstr0("HOOKED") }}},
        .height = {{ SZEXP_VAR_H, { .varname = bstr0("HOOKED") }}},
        .cond = {{ SZEXP_CONST, { .cval = 1.0 }}},
    };

    int hook_idx = 0;
    int bind_idx = 0;

    // Skip all garbage (e.g. comments) before the first header
    int pos = bstr_find(*body, bstr0("//!"));
    if (pos < 0) {
        mp_warn(log, "Shader appears to contain no passes!\n");
        return false;
    }
    *body = bstr_cut(*body, pos);

    // First parse all the headers
    while (true) {
        struct bstr rest;
        struct bstr line = bstr_strip(bstr_getline(*body, &rest));

        // Check for the presence of the magic line beginning
        if (!bstr_eatstart0(&line, "//!"))
            break;

        *body = rest;

        // Parse the supported commands
        if (bstr_eatstart0(&line, "HOOK")) {
            if (hook_idx == SHADER_MAX_HOOKS) {
                mp_err(log, "Passes may only hook up to %d textures!\n",
                       SHADER_MAX_HOOKS);
                return false;
            }
            out->hook_tex[hook_idx++] = bstr_strip(line);
            continue;
        }

        if (bstr_eatstart0(&line, "BIND")) {
            if (bind_idx == SHADER_MAX_BINDS) {
                mp_err(log, "Passes may only bind up to %d textures!\n",
                       SHADER_MAX_BINDS);
                return false;
            }
            out->bind_tex[bind_idx++] = bstr_strip(line);
            continue;
        }

        if (bstr_eatstart0(&line, "SAVE")) {
            out->save_tex = bstr_strip(line);
            continue;
        }

        if (bstr_eatstart0(&line, "OFFSET")) {
            float ox, oy;
            if (bstr_sscanf(line, "%f %f", &ox, &oy) != 2) {
                mp_err(log, "Error while parsing OFFSET!\n");
                return false;
            }
            out->offset.t[0] = ox;
            out->offset.t[1] = oy;
            continue;
        }

        if (bstr_eatstart0(&line, "WIDTH")) {
            if (!parse_rpn_szexpr(line, out->width)) {
                mp_err(log, "Error while parsing WIDTH!\n");
                return false;
            }
            continue;
        }

        if (bstr_eatstart0(&line, "HEIGHT")) {
            if (!parse_rpn_szexpr(line, out->height)) {
                mp_err(log, "Error while parsing HEIGHT!\n");
                return false;
            }
            continue;
        }

        if (bstr_eatstart0(&line, "WHEN")) {
            if (!parse_rpn_szexpr(line, out->cond)) {
                mp_err(log, "Error while parsing WHEN!\n");
                return false;
            }
            continue;
        }

        if (bstr_eatstart0(&line, "COMPONENTS")) {
            if (bstr_sscanf(line, "%d", &out->components) != 1) {
                mp_err(log, "Error while parsing COMPONENTS!\n");
                return false;
            }
            continue;
        }

        // Unknown command type
        mp_err(log, "Unrecognized command '%.*s'!\n", BSTR_P(line));
        return false;
    }

    // The rest of the file up until the next magic line beginning (if any)
    // shall be the shader body
    if (bstr_split_tok(*body, "//!", &out->pass_body, body)) {
        // Make sure the magic line is part of the rest
        body->start -= 3;
        body->len += 3;
    }

    // Sanity checking
    if (hook_idx == 0)
        mp_warn(log, "Pass has no hooked textures (will be ignored)!\n");

    return true;
}
