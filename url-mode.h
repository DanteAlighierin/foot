#pragma once

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>
#include <tllist.h>

#include "config.h"
#include "key-binding.h"
#include "terminal.h"

static inline bool urls_mode_is_active(const struct terminal *term)
{
    return tll_length(term->urls) > 0;
}

void urls_collect(
    const struct terminal *term, enum url_action action, url_list_t *urls);
void urls_assign_key_combos(const struct config *conf, url_list_t *urls);

void urls_render(struct terminal *term);
void urls_reset(struct terminal *term);

void urls_input(struct seat *seat, struct terminal *term,
                const struct key_binding_set *bindings, uint32_t key,
                xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
                xkb_mod_mask_t locked,
                const xkb_keysym_t *raw_syms, size_t raw_count,
                uint32_t serial);
