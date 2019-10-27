#include "wayland.h"

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-shell.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <xdg-output-unstable-v1.h>
#include <xdg-decoration-unstable-v1.h>

#define LOG_MODULE "wayland"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "tllist.h"

void
wayl_init(struct wayland *wayl)
{
}

void
wayl_destroy(struct wayland *wayl)
{
    tll_foreach(wayl->monitors, it) {
        free(it->item.name);
        if (it->item.xdg != NULL)
            zxdg_output_v1_destroy(it->item.xdg);
        if (it->item.output != NULL)
            wl_output_destroy(it->item.output);
        tll_remove(wayl->monitors, it);
    }

    if (wayl->xdg_output_manager != NULL)
        zxdg_output_manager_v1_destroy(wayl->xdg_output_manager);

    if (wayl->clipboard.data_source != NULL)
        wl_data_source_destroy(wayl->clipboard.data_source);
    if (wayl->clipboard.data_offer != NULL)
        wl_data_offer_destroy(wayl->clipboard.data_offer);
    free(wayl->clipboard.text);
    if (wayl->primary.data_source != NULL)
        zwp_primary_selection_source_v1_destroy(wayl->primary.data_source);
    if (wayl->primary.data_offer != NULL)
        zwp_primary_selection_offer_v1_destroy(wayl->primary.data_offer);
    free(wayl->primary.text);

    free(wayl->pointer.theme_name);
    if (wayl->pointer.theme != NULL)
        wl_cursor_theme_destroy(wayl->pointer.theme);
    if (wayl->pointer.pointer != NULL)
        wl_pointer_destroy(wayl->pointer.pointer);
    if (wayl->pointer.surface != NULL)
        wl_surface_destroy(wayl->pointer.surface);
    if (wayl->keyboard != NULL)
        wl_keyboard_destroy(wayl->keyboard);
    if (wayl->data_device != NULL)
        wl_data_device_destroy(wayl->data_device);
    if (wayl->data_device_manager != NULL)
        wl_data_device_manager_destroy(wayl->data_device_manager);
    if (wayl->primary_selection_device != NULL)
        zwp_primary_selection_device_v1_destroy(wayl->primary_selection_device);
    if (wayl->primary_selection_device_manager != NULL)
        zwp_primary_selection_device_manager_v1_destroy(wayl->primary_selection_device_manager);
    if (wayl->seat != NULL)
        wl_seat_destroy(wayl->seat);
    if (wayl->shm != NULL)
        wl_shm_destroy(wayl->shm);
    if (wayl->sub_compositor != NULL)
        wl_subcompositor_destroy(wayl->sub_compositor);
    if (wayl->compositor != NULL)
        wl_compositor_destroy(wayl->compositor);
    if (wayl->registry != NULL)
        wl_registry_destroy(wayl->registry);
    if (wayl->display != NULL)
        wl_display_disconnect(wayl->display);
}

void
wayl_win_destroy(struct wl_window *win)
{
    tll_free(win->on_outputs);
    if (win->search_sub_surface != NULL)
        wl_subsurface_destroy(win->search_sub_surface);
    if (win->search_surface != NULL)
        wl_surface_destroy(win->search_surface);
    if (win->frame_callback != NULL)
        wl_callback_destroy(win->frame_callback);
    if (win->xdg_toplevel_decoration != NULL)
        zxdg_toplevel_decoration_v1_destroy(win->xdg_toplevel_decoration);
    if (win->xdg_decoration_manager != NULL)
        zxdg_decoration_manager_v1_destroy(win->xdg_decoration_manager);
    if (win->xdg_toplevel != NULL)
        xdg_toplevel_destroy(win->xdg_toplevel);
    if (win->xdg_surface != NULL)
        xdg_surface_destroy(win->xdg_surface);
    if (win->shell != NULL)
        xdg_wm_base_destroy(win->shell);
    if (win->surface != NULL)
        wl_surface_destroy(win->surface);
}
