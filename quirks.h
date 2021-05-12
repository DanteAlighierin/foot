#pragma once

#include <wayland-client.h>

#include "terminal.h"

/*
 * On weston (8.0), synchronized subsurfaces aren't updated correctly.

 * They appear to render once, but after that, updates are
 * sporadic. Sometimes they update, most of the time they don't.
 *
 * Adding explicit parent surface commits right after the subsurface
 * commit doesn't help (and would be useless anyway, since it would
 * defeat the purpose of having the subsurface synchronized in the
 * first place).
 */
void quirk_weston_subsurface_desync_on(struct wl_subsurface *sub);
void quirk_weston_subsurface_desync_off(struct wl_subsurface *sub);

/* Shortcuts to call desync_{on,off} on all CSD subsurfaces */
void quirk_weston_csd_on(struct terminal *term);
void quirk_weston_csd_off(struct terminal *term);
