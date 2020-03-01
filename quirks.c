#include "quirks.h"

#include <stdlib.h>
#include <stdbool.h>

static bool
is_weston(void)
{
    /*
     * On weston (8.0), synchronized subsurfaces aren't updated
     * correctly.

     * They appear to render once, but after that, updates are
     * sporadic. Sometimes they update, most of the time they
     * don't.
     *
     * Adding explicit parent surface commits right after the
     * subsurface commit doesn't help (and would be useless anyway,
     * since it would defeat the purpose of having the subsurface
     * synchronized in the first place).
     */
    static bool is_weston = false;
    static bool initialized = false;

    if (!initialized) {
        initialized = true;
        is_weston = getenv("WESTON_CONFIG_FILE") != NULL;
    }

    return is_weston;
}

void
quirk_weston_subsurface_desync_on(struct wl_subsurface *sub)
{
    if (!is_weston())
        return;

    wl_subsurface_set_desync(sub);
}

void
quirk_weston_subsurface_desync_off(struct wl_subsurface *sub)
{
    if (!is_weston())
        return;

    wl_subsurface_set_sync(sub);
}
