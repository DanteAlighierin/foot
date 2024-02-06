#pragma once

#include <xkbcommon/xkbcommon-keysyms.h>

#include "wayland.h"

void unicode_mode_activate(struct seat *seat);
void unicode_mode_deactivate(struct seat *seat);
void unicode_mode_updated(struct seat *seat);
void unicode_mode_input(struct seat *seat, struct terminal *term,
                        xkb_keysym_t sym);
