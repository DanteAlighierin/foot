#pragma once

#include <stdint.h>
#include <wayland-client.h>

#include "wayland.h"

extern const struct wl_keyboard_listener keyboard_listener;
extern const struct wl_pointer_listener pointer_listener;

void input_repeat(struct seat *seat, uint32_t key);
