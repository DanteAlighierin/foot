#pragma once

#include <xkbcommon/xkbcommon-keysyms.h>

#include "terminal.h"

void unicode_mode_activate(struct terminal *term);
void unicode_mode_deactivate(struct terminal *term);
void unicode_mode_updated(struct terminal *term);
void unicode_mode_input(struct seat *seat, struct terminal *term,
                        xkb_keysym_t sym);
