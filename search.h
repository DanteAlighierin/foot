#pragma once

#include <xkbcommon/xkbcommon.h>

#include "key-binding.h"
#include "terminal.h"

void search_begin(struct terminal *term);
void search_cancel(struct terminal *term);
void search_input(
    struct seat *seat, struct terminal *term,
    const struct key_binding_set *bindings, uint32_t key,
    xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
    xkb_mod_mask_t locked,
    const xkb_keysym_t *raw_syms, size_t raw_count,
    uint32_t serial);
void search_add_chars(struct terminal *term, const char *text, size_t len);

void search_selection_cancelled(struct terminal *term);

struct search_match_iterator {
    struct terminal *term;
    struct coord start;
};

struct search_match_iterator search_matches_new_iter(struct terminal *term);
struct range search_matches_next(struct search_match_iterator *iter);
