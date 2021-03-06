#pragma once

#include <xkbcommon/xkbcommon.h>
#include "terminal.h"

void search_begin(struct terminal *term);
void search_cancel(struct terminal *term);
void search_input(
    struct seat *seat, struct terminal *term, uint32_t key,
    xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
    const xkb_keysym_t *raw_syms, size_t raw_count,
    uint32_t serial);
void search_add_chars(struct terminal *term, const char *text, size_t len);
