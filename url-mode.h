#pragma once

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>
#include <tllist.h>

#include "terminal.h"

static inline bool urls_mode_is_active(const struct terminal *term)
{
    return tll_length(term->urls) > 0;
}

void urls_collect(struct terminal *term, enum url_action action);
void urls_tag_cells(struct terminal *term);
void urls_reset(struct terminal *term);

void urls_input(struct seat *seat, struct terminal *term, uint32_t key,
                xkb_keysym_t sym, xkb_mod_mask_t mods, uint32_t serial);
