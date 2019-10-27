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
