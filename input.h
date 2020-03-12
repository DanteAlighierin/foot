#pragma once

#include <stdint.h>
#include <wayland-client.h>

#include "wayland.h"

extern const struct wl_keyboard_listener keyboard_listener;
extern const struct wl_pointer_listener pointer_listener;

void input_repeat(struct wayland *wayl, uint32_t key);

bool input_parse_key_binding_for_action(
    struct xkb_keymap *keymap, enum binding_action action,
    const char *combos, key_binding_list_t *bindings);

void input_execute_binding(
    struct terminal *term, enum binding_action action, uint32_t serial);
