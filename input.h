#pragma once

#include <wayland-client.h>

#include "terminal.h"

extern const struct wl_keyboard_listener keyboard_listener;
extern const struct wl_pointer_listener pointer_listener;

void input_repeat(struct terminal *term, uint32_t key);
