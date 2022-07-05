#pragma once

#include <stdint.h>
#include <wayland-client.h>

#include "wayland.h"
#include "misc.h"

/*
 * Custom defines for mouse wheel left/right buttons.
 *
 * Libinput does not define these. On Wayland, all scroll events (both
 * vertical and horizontal) are reported not as buttons, as ‘axis’
 * events.
 *
 * Libinput _does_ define BTN_BACK and BTN_FORWARD, which is
 * what we use for vertical scroll events. But for horizontal scroll
 * events, there aren’t any pre-defined mouse buttons.
 *
 * Mouse buttons are in the range 0x110 - 0x11f, with joystick defines
 * starting at 0x120.
 */
#define BTN_WHEEL_LEFT 0x11e
#define BTN_WHEEL_RIGHT 0x11f

extern const struct wl_keyboard_listener keyboard_listener;
extern const struct wl_pointer_listener pointer_listener;

void input_repeat(struct seat *seat, uint32_t key);

void get_current_modifiers(const struct seat *seat,
                           xkb_mod_mask_t *effective,
                           xkb_mod_mask_t *consumed,
                           uint32_t key);

const char *xcursor_for_csd_border(struct terminal *term, int x, int y);
