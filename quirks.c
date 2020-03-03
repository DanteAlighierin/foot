#include "quirks.h"

#include <stdlib.h>
#include <stdbool.h>

#define LOG_MODULE "quirks"
#define LOG_ENABLE_DBG 0
#include "log.h"

#define ALEN(v) (sizeof(v) / sizeof(v[0]))

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
        if (is_weston)
            LOG_WARN("applying wl_subsurface_set_desync() workaround for weston");
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

void
quirk_weston_csd_on(struct terminal *term)
{
    if (term->window->use_csd != CSD_YES)
        return;

    for (int i = 0; i < ALEN(term->window->csd.surface); i++)
        quirk_weston_subsurface_desync_on(term->window->csd.sub_surface[i]);
}

void
quirk_weston_csd_off(struct terminal *term)
{
    if (term->window->use_csd != CSD_YES)
        return;

    for (int i = 0; i < ALEN(term->window->csd.surface); i++)
        quirk_weston_subsurface_desync_off(term->window->csd.sub_surface[i]);
}
