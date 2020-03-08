#pragma once

#include <xkbcommon/xkbcommon.h>
#include "terminal.h"

void search_begin(struct terminal *term);
void search_cancel(struct terminal *term);
void search_input(struct terminal *term, uint32_t key, xkb_keysym_t sym, xkb_mod_mask_t mods,
                  uint32_t serial);
