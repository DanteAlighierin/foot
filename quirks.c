#include "quirks.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define LOG_MODULE "quirks"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "util.h"

static bool
is_weston(void)
{
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
    if (term->window->is_fullscreen)
        return;

    for (int i = 0; i < ALEN(term->window->csd.surface); i++)
        quirk_weston_subsurface_desync_on(term->window->csd.sub_surface[i]);
}

void
quirk_weston_csd_off(struct terminal *term)
{
    if (term->window->use_csd != CSD_YES)
        return;
    if (term->window->is_fullscreen)
        return;

    for (int i = 0; i < ALEN(term->window->csd.surface); i++)
        quirk_weston_subsurface_desync_off(term->window->csd.sub_surface[i]);
}

void
quirk_kde_damage_before_attach(struct wl_surface *surface)
{
    static bool is_kde = false;
    static bool initialized = false;

    if (!initialized) {
        initialized = true;

        const char *cur_desktop = getenv("XDG_CURRENT_DESKTOP");
        if (cur_desktop != NULL)
            is_kde = strcasestr(cur_desktop, "kde") != NULL;

        if (is_kde)
            LOG_WARN("applying wl_surface_damage_buffer() workaround for KDE");
    }

    if (!is_kde)
        return;

    wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
}
