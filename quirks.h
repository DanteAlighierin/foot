#pragma once

#include <wayland-client.h>

void quirk_weston_subsurface_desync_on(struct wl_subsurface *sub);
void quirk_weston_subsurface_desync_off(struct wl_subsurface *sub);
