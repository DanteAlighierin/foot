#include "wayland.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/timerfd.h>
#include <sys/epoll.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-shell.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <tllist.h>
#include <xdg-output-unstable-v1.h>
#include <xdg-decoration-unstable-v1.h>

#define LOG_MODULE "wayland"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "config.h"
#include "terminal.h"
#include "input.h"
#include "render.h"
#include "selection.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

static bool wayl_reload_cursor_theme(
    struct wayland *wayl, const struct terminal *term);

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
    struct wayland *wayl = data;
    if (format == WL_SHM_FORMAT_ARGB8888)
        wayl->have_argb8888 = true;
}

static const struct wl_shm_listener shm_listener = {
    .format = &shm_format,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    LOG_DBG("wm base ping");
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = &xdg_wm_base_ping,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
                         enum wl_seat_capability caps)
{
    struct wayland *wayl = data;

    if (wayl->keyboard != NULL) {
        wl_keyboard_release(wayl->keyboard);
        wayl->keyboard = NULL;
    }

    if (wayl->pointer.pointer != NULL) {
        wl_pointer_release(wayl->pointer.pointer);
        wayl->pointer.pointer = NULL;
    }

    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        wayl->keyboard = wl_seat_get_keyboard(wl_seat);
        wl_keyboard_add_listener(wayl->keyboard, &keyboard_listener, wayl);
    }

    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        wayl->pointer.pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(wayl->pointer.pointer, &pointer_listener, wayl);
    }
}

static void
seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

static void
output_update_ppi(struct monitor *mon)
{
    int x_inches = mon->width_mm * 0.03937008;
    int y_inches = mon->height_mm * 0.03937008;
    mon->x_ppi = mon->width_px / x_inches;
    mon->y_ppi = mon->height_px / y_inches;
}

static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                int32_t physical_width, int32_t physical_height,
                int32_t subpixel, const char *make, const char *model,
                int32_t transform)
{
    struct monitor *mon = data;
    mon->width_mm = physical_width;
    mon->height_mm = physical_height;
    mon->inch = sqrt(pow(mon->width_mm, 2) + pow(mon->height_mm, 2)) * 0.03937008;
    mon->make = make != NULL ? strdup(make) : NULL;
    mon->model = model != NULL ? strdup(model) : NULL;
    output_update_ppi(mon);
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
            int32_t width, int32_t height, int32_t refresh)
{
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)
        return;

    struct monitor *mon = data;
    mon->refresh = (float)refresh / 1000;
}

static void
output_done(void *data, struct wl_output *wl_output)
{
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    struct monitor *mon = data;

    mon->scale = factor;

    tll_foreach(mon->wayl->terms, it) {
        struct terminal *term = it->item;
        render_resize(term, term->width / term->scale, term->height / term->scale);
        wayl_reload_cursor_theme(mon->wayl, term);
    }
}

static const struct wl_output_listener output_listener = {
    .geometry = &output_geometry,
    .mode = &output_mode,
    .done = &output_done,
    .scale = &output_scale,
};

static void
xdg_output_handle_logical_position(
    void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y)
{
    struct monitor *mon = data;
    mon->x = x;
    mon->y = y;
}

static void
xdg_output_handle_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
                               int32_t width, int32_t height)
{
    struct monitor *mon = data;
    mon->width_px = width;
    mon->height_px = height;
    output_update_ppi(mon);
}

static void
xdg_output_handle_done(void *data, struct zxdg_output_v1 *xdg_output)
{
}

static void
xdg_output_handle_name(void *data, struct zxdg_output_v1 *xdg_output,
                       const char *name)
{
    struct monitor *mon = data;
    mon->name = strdup(name);
}

static void
xdg_output_handle_description(void *data, struct zxdg_output_v1 *xdg_output,
                              const char *description)
{
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_handle_logical_position,
    .logical_size = xdg_output_handle_logical_size,
    .done = xdg_output_handle_done,
    .name = xdg_output_handle_name,
    .description = xdg_output_handle_description,
};

static void
clock_id(void *data, struct wp_presentation *wp_presentation, uint32_t clk_id)
{
    struct wayland *wayl = data;
    wayl->presentation_clock_id = clk_id;
    LOG_DBG("presentation clock ID: %u", clk_id);
}

static const struct wp_presentation_listener presentation_listener = {
    .clock_id = &clock_id,
};


static bool
verify_iface_version(const char *iface, uint32_t version, uint32_t wanted)
{
    if (version >= wanted)
        return true;

    LOG_ERR("%s: need interface version %u, but compositor only implements %u",
            iface, wanted, version);
    return false;
}

static void
handle_global(void *data, struct wl_registry *registry,
              uint32_t name, const char *interface, uint32_t version)
{
    LOG_DBG("global: %s, version=%u", interface, version);
    struct wayland *wayl = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        const uint32_t required = 4;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->compositor = wl_registry_bind(
            wayl->registry, name, &wl_compositor_interface, required);
    }

    else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->sub_compositor = wl_registry_bind(
            wayl->registry, name, &wl_subcompositor_interface, required);
    }

    else if (strcmp(interface, wl_shm_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->shm = wl_registry_bind(
            wayl->registry, name, &wl_shm_interface, required);
        wl_shm_add_listener(wayl->shm, &shm_listener, wayl);
        wl_display_roundtrip(wayl->display);
    }

    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->shell = wl_registry_bind(
            wayl->registry, name, &xdg_wm_base_interface, required);
        xdg_wm_base_add_listener(wayl->shell, &xdg_wm_base_listener, wayl);
    }

    else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->xdg_decoration_manager = wl_registry_bind(
            wayl->registry, name, &zxdg_decoration_manager_v1_interface, required);
    }

    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        const uint32_t required = 5;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->seat = wl_registry_bind(
            wayl->registry, name, &wl_seat_interface, required);
        wl_seat_add_listener(wayl->seat, &seat_listener, wayl);
        wl_display_roundtrip(wayl->display);
    }

    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->xdg_output_manager = wl_registry_bind(
            wayl->registry, name, &zxdg_output_manager_v1_interface,
            min(version, 2));
    }

    else if (strcmp(interface, wl_output_interface.name) == 0) {
        const uint32_t required = 3;
        if (!verify_iface_version(interface, version, required))
            return;

        struct wl_output *output = wl_registry_bind(
            wayl->registry, name, &wl_output_interface, required);

        tll_push_back(
            wayl->monitors, ((struct monitor){.wayl = wayl, .output = output}));

        struct monitor *mon = &tll_back(wayl->monitors);
        wl_output_add_listener(output, &output_listener, mon);

        if (wayl->xdg_output_manager != NULL) {
            mon->xdg = zxdg_output_manager_v1_get_xdg_output(
                wayl->xdg_output_manager, mon->output);
            zxdg_output_v1_add_listener(mon->xdg, &xdg_output_listener, mon);
        }
        wl_display_roundtrip(wayl->display);
    }

    else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->data_device_manager = wl_registry_bind(
            wayl->registry, name, &wl_data_device_manager_interface, required);
    }

    else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->primary_selection_device_manager = wl_registry_bind(
            wayl->registry, name,
            &zwp_primary_selection_device_manager_v1_interface, required);
    }

    else if (strcmp(interface, wp_presentation_interface.name) == 0) {
        if (wayl->conf->presentation_timings) {
            const uint32_t required = 1;
            if (!verify_iface_version(interface, version, required))
                return;

            wayl->presentation = wl_registry_bind(
                wayl->registry, name, &wp_presentation_interface, required);
            wp_presentation_add_listener(
                wayl->presentation, &presentation_listener, wayl);
        }
    }
}

static void
surface_enter(void *data, struct wl_surface *wl_surface,
              struct wl_output *wl_output)
{
    struct wayland *wayl = data;
    struct terminal *term = wayl_terminal_from_surface(wayl, wl_surface);

    tll_foreach(wayl->monitors, it) {
        if (it->item.output == wl_output) {
            LOG_DBG("mapped on %s", it->item.name);
            tll_push_back(term->window->on_outputs, &it->item);

            /* Resize, since scale-to-use may have changed */
            render_resize(term, term->width / term->scale, term->height / term->scale);
            wayl_reload_cursor_theme(wayl, term);
            return;
        }
    }

    LOG_ERR("mapped on unknown output");
}

static void
surface_leave(void *data, struct wl_surface *wl_surface,
              struct wl_output *wl_output)
{
    struct wayland *wayl = data;
    struct terminal *term = wayl_terminal_from_surface(wayl, wl_surface);

    tll_foreach(term->window->on_outputs, it) {
        if (it->item->output != wl_output)
            continue;

        LOG_DBG("unmapped from %s", it->item->name);
        tll_remove(term->window->on_outputs, it);

        /* Resize, since scale-to-use may have changed */
        render_resize(term, term->width / term->scale, term->height / term->scale);
        wayl_reload_cursor_theme(wayl, term);
        return;
    }

    LOG_ERR("unmapped from unknown output");
}

static const struct wl_surface_listener surface_listener = {
    .enter = &surface_enter,
    .leave = &surface_leave,
};

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                       int32_t width, int32_t height, struct wl_array *states)
{
    LOG_DBG("xdg-toplevel: configure: %dx%d", width, height);

    if (width == 0 && height == 0)
        return;

    struct wayland *wayl = data;
    struct terminal *term = wayl_terminal_from_xdg_toplevel(wayl, xdg_toplevel);

    bool is_focused = false;
    enum xdg_toplevel_state *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_ACTIVATED) {
            is_focused = true;
        }
    }

    term->visual_focus = is_focused;
    render_resize(term, width, height);
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    struct wayland *wayl = data;
    struct terminal *term = wayl_terminal_from_xdg_toplevel(wayl, xdg_toplevel);
    LOG_DBG("xdg-toplevel: close");
    term_shutdown(term);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = &xdg_toplevel_configure,
    .close = &xdg_toplevel_close,
};

static void
xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                      uint32_t serial)
{
    //LOG_DBG("xdg-surface: configure");
    xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = &xdg_surface_configure,
};

static void
xdg_toplevel_decoration_configure(void *data,
                                  struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1,
                                  uint32_t mode)
{
    switch (mode) {
    case ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
        LOG_ERR("unimplemented: client-side decorations");
        break;

    case ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
        LOG_DBG("using server-side decorations");
        break;

    default:
        LOG_ERR("unimplemented: unknown XDG toplevel decoration mode: %u", mode);
        break;
    }
}

static const struct zxdg_toplevel_decoration_v1_listener xdg_toplevel_decoration_listener = {
    .configure = &xdg_toplevel_decoration_configure,
};

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    LOG_WARN("global removed: %u", name);
    assert(false);
}

static const struct wl_registry_listener registry_listener = {
    .global = &handle_global,
    .global_remove = &handle_global_remove,
};

static bool
fdm_wayl(struct fdm *fdm, int fd, int events, void *data)
{
    struct wayland *wayl = data;
    int event_count = wl_display_dispatch(wayl->display);

    if (events & EPOLLHUP) {
        LOG_WARN("disconnected from Wayland");
        return false;
    }

    wl_display_flush(wayl->display);
    return event_count != -1;
}

static bool
fdm_repeat(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct wayland *wayl = data;
    uint64_t expiration_count;
    ssize_t ret = read(
        wayl->kbd.repeat.fd, &expiration_count, sizeof(expiration_count));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read repeat key from repeat timer fd");
        return false;
    }

    wayl->kbd.repeat.dont_re_repeat = true;
    for (size_t i = 0; i < expiration_count; i++)
        input_repeat(wayl, wayl->kbd.repeat.key);
    wayl->kbd.repeat.dont_re_repeat = false;
    return true;
}

struct wayland *
wayl_init(const struct config *conf, struct fdm *fdm)
{
    struct wayland *wayl = calloc(1, sizeof(*wayl));
    wayl->conf = conf;
    wayl->fdm = fdm;
    wayl->kbd.repeat.fd = -1;

    wayl->display = wl_display_connect(NULL);
    if (wayl->display == NULL) {
        LOG_ERR("failed to connect to wayland; no compositor running?");
        goto out;
    }

    wayl->registry = wl_display_get_registry(wayl->display);
    if (wayl->registry == NULL) {
        LOG_ERR("failed to get wayland registry");
        goto out;
    }

    wl_registry_add_listener(wayl->registry, &registry_listener, wayl);
    wl_display_roundtrip(wayl->display);

    if (wayl->compositor == NULL) {
        LOG_ERR("no compositor");
        goto out;
    }
    if (wayl->sub_compositor == NULL) {
        LOG_ERR("no sub compositor");
        goto out;
    }
    if (wayl->shm == NULL) {
        LOG_ERR("no shared memory buffers interface");
        goto out;
    }
    if (wayl->shell == NULL) {
        LOG_ERR("no XDG shell interface");
        goto out;
    }
    if (wayl->seat == NULL) {
        LOG_ERR("no seat available");
        goto out;
    }
    if (wayl->data_device_manager == NULL) {
        LOG_ERR("no clipboard available "
                "(wl_data_device_manager not implemented by server)");
        goto out;
    }
    if (!wayl->have_argb8888) {
        LOG_ERR("compositor does not support ARGB surfaces");
        goto out;
    }

    if (wayl->primary_selection_device_manager == NULL)
        LOG_WARN("no primary selection available");

    if (conf->presentation_timings && wayl->presentation == NULL)
        LOG_WARN("presentation time interface not implemented by compositor; "
                 "timings will not be available");

    tll_foreach(wayl->monitors, it) {
        LOG_INFO(
            "%s: %dx%d+%dx%d@%dHz %s (%.2f\", PPI=%dx%d, scale=%d)",
            it->item.name, it->item.width_px, it->item.height_px,
            it->item.x, it->item.y, (int)round(it->item.refresh), it->item.model, it->item.inch,
            it->item.x_ppi, it->item.y_ppi,
            it->item.scale);
    }

    /* Clipboard */
    wayl->data_device = wl_data_device_manager_get_data_device(
        wayl->data_device_manager, wayl->seat);
    wl_data_device_add_listener(wayl->data_device, &data_device_listener, wayl);

    /* Primary selection */
    if (wayl->primary_selection_device_manager != NULL) {
        wayl->primary_selection_device = zwp_primary_selection_device_manager_v1_get_device(
            wayl->primary_selection_device_manager, wayl->seat);
        zwp_primary_selection_device_v1_add_listener(
            wayl->primary_selection_device, &primary_selection_device_listener, wayl);
    }

    /* Cursor */
    unsigned cursor_size = 24;
    const char *cursor_theme = getenv("XCURSOR_THEME");

    {
        const char *env_cursor_size = getenv("XCURSOR_SIZE");
        if (env_cursor_size != NULL) {
            unsigned size;
            if (sscanf(env_cursor_size, "%u", &size) == 1)
                cursor_size = size;
        }
    }

    /* Note: theme is (re)loaded on scale and output changes */
    LOG_INFO("cursor theme: %s, size: %u", cursor_theme, cursor_size);
    wayl->pointer.size = cursor_size;
    wayl->pointer.theme_name = cursor_theme != NULL ? strdup(cursor_theme) : NULL;

    wayl->pointer.surface = wl_compositor_create_surface(wayl->compositor);
    if (wayl->pointer.surface == NULL) {
        LOG_ERR("failed to create cursor surface");
        goto out;
    }

    /* All wayland initialization done - make it so */
    wl_display_roundtrip(wayl->display);

    wayl->kbd.repeat.fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK);
    if (wayl->kbd.repeat.fd == -1) {
        LOG_ERRNO("failed to create keyboard repeat timer FD");
        goto out;
    }

    if (!fdm_add(fdm, wl_display_get_fd(wayl->display), EPOLLIN, &fdm_wayl, wayl) ||
        !fdm_add(fdm, wayl->kbd.repeat.fd, EPOLLIN, &fdm_repeat, wayl))
    {
        goto out;
    }

    return wayl;

out:
    if (wayl != NULL)
        wayl_destroy(wayl);
    return NULL;
}

void
wayl_destroy(struct wayland *wayl)
{
    if (wayl == NULL)
        return;

    tll_foreach(wayl->terms, it) {
        static bool have_warned = false;
        if (!have_warned) {
            have_warned = true;
            LOG_WARN("there are terminals still running");
            term_destroy(it->item);
        }
    }

    tll_free(wayl->terms);

    if (wayl->kbd.repeat.fd != -1)
        fdm_del(wayl->fdm, wayl->kbd.repeat.fd);

    tll_foreach(wayl->monitors, it) {
        free(it->item.name);
        if (it->item.xdg != NULL)
            zxdg_output_v1_destroy(it->item.xdg);
        if (it->item.output != NULL)
            wl_output_destroy(it->item.output);
        free(it->item.make);
        free(it->item.model);
        tll_remove(wayl->monitors, it);
    }

    if (wayl->xdg_output_manager != NULL)
        zxdg_output_manager_v1_destroy(wayl->xdg_output_manager);
    if (wayl->shell != NULL)
        xdg_wm_base_destroy(wayl->shell);

    if (wayl->xdg_decoration_manager != NULL)
        zxdg_decoration_manager_v1_destroy(wayl->xdg_decoration_manager);

    if (wayl->presentation != NULL)
        wp_presentation_destroy(wayl->presentation);

    if (wayl->kbd.xkb_compose_state != NULL)
        xkb_compose_state_unref(wayl->kbd.xkb_compose_state);
    if (wayl->kbd.xkb_compose_table != NULL)
        xkb_compose_table_unref(wayl->kbd.xkb_compose_table);
    if (wayl->kbd.xkb_keymap != NULL)
        xkb_keymap_unref(wayl->kbd.xkb_keymap);
    if (wayl->kbd.xkb_state != NULL)
        xkb_state_unref(wayl->kbd.xkb_state);
    if (wayl->kbd.xkb != NULL)
        xkb_context_unref(wayl->kbd.xkb);

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
    if (wayl->display != NULL) {
        fdm_del_no_close(wayl->fdm, wl_display_get_fd(wayl->display));
        wl_display_disconnect(wayl->display);
    }

    free(wayl);
}

struct wl_window *
wayl_win_init(struct wayland *wayl)
{
    struct wl_window *win = calloc(1, sizeof(*win));
    win->wayl = wayl;

    win->surface = wl_compositor_create_surface(wayl->compositor);
    if (win->surface == NULL) {
        LOG_ERR("failed to create wayland surface");
        goto out;
    }

    wl_surface_add_listener(win->surface, &surface_listener, wayl);

    win->xdg_surface = xdg_wm_base_get_xdg_surface(wayl->shell, win->surface);
    xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, wayl);

    win->xdg_toplevel = xdg_surface_get_toplevel(win->xdg_surface);
    xdg_toplevel_add_listener(win->xdg_toplevel, &xdg_toplevel_listener, wayl);

    xdg_toplevel_set_app_id(win->xdg_toplevel, "foot");

    /* Request server-side decorations */
    win->xdg_toplevel_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
        wayl->xdg_decoration_manager, win->xdg_toplevel);
    zxdg_toplevel_decoration_v1_set_mode(
        win->xdg_toplevel_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    zxdg_toplevel_decoration_v1_add_listener(
        win->xdg_toplevel_decoration, &xdg_toplevel_decoration_listener, wayl);

    /* Scrollback search box */
    win->search_surface = wl_compositor_create_surface(wayl->compositor);
    win->search_sub_surface = wl_subcompositor_get_subsurface(
        wayl->sub_compositor, win->search_surface, win->surface);
    wl_subsurface_set_desync(win->search_sub_surface);

    wl_surface_commit(win->surface);
    wl_display_roundtrip(wayl->display);

    return win;

out:
    if (win != NULL)
        wayl_win_destroy(win);
    return NULL;
}

void
wayl_win_destroy(struct wl_window *win)
{
    if (win == NULL)
        return;

    /*
     * First, unmap all surfaces to trigger things like
     * keyboard_leave() and wl_pointer_leave().
     *
     * This ensures we remove all references to *this* window from the
     * global wayland struct (since it no longer has neither keyboard
     * nor mouse focus).
     */

    /* Scrollback search */
    wl_surface_attach(win->search_surface, NULL, 0, 0);
    wl_surface_commit(win->search_surface);
    wl_display_roundtrip(win->wayl->display);

    /* Main window */
    wl_surface_attach(win->surface, NULL, 0, 0);
    wl_surface_commit(win->surface);
    wl_display_roundtrip(win->wayl->display);

    tll_free(win->on_outputs);
    if (win->search_sub_surface != NULL)
        wl_subsurface_destroy(win->search_sub_surface);
    if (win->search_surface != NULL)
        wl_surface_destroy(win->search_surface);
    if (win->frame_callback != NULL)
        wl_callback_destroy(win->frame_callback);
    if (win->xdg_toplevel_decoration != NULL)
        zxdg_toplevel_decoration_v1_destroy(win->xdg_toplevel_decoration);
    if (win->xdg_toplevel != NULL)
        xdg_toplevel_destroy(win->xdg_toplevel);
    if (win->xdg_surface != NULL)
        xdg_surface_destroy(win->xdg_surface);
    if (win->surface != NULL)
        wl_surface_destroy(win->surface);

    wl_display_roundtrip(win->wayl->display);

    free(win);
}

bool
wayl_cursor_set(struct wayland *wayl, const struct terminal *term)
{
    if (wayl->pointer.theme == NULL)
        return false;

    if (wayl->mouse_focus == NULL) {
        wayl->pointer.xcursor = NULL;
        return true;
    }

    if (wayl->mouse_focus != term) {
        /* This terminal doesn't have mouse focus */
        return true;
    }

    if (wayl->pointer.xcursor == term->xcursor)
        return true;

    wayl->pointer.cursor = wl_cursor_theme_get_cursor(wayl->pointer.theme, term->xcursor);
    if (wayl->pointer.cursor == NULL) {
        LOG_ERR("%s: failed to load xcursor pointer '%s'",
                wayl->pointer.theme_name, term->xcursor);
        return false;
    }

    wayl->pointer.xcursor = term->xcursor;

    const int scale = term->scale;
    struct wl_cursor_image *image = wayl->pointer.cursor->images[0];

    wl_surface_attach(
        wayl->pointer.surface, wl_cursor_image_get_buffer(image), 0, 0);

    wl_pointer_set_cursor(
        wayl->pointer.pointer, wayl->pointer.serial,
        wayl->pointer.surface,
        image->hotspot_x / scale, image->hotspot_y / scale);

    wl_surface_damage_buffer(
        wayl->pointer.surface, 0, 0, INT32_MAX, INT32_MAX);

    wl_surface_set_buffer_scale(wayl->pointer.surface, scale);
    wl_surface_commit(wayl->pointer.surface);
    return true;
}

static bool
wayl_reload_cursor_theme(struct wayland *wayl, const struct terminal *term)
{
    if (wayl->pointer.size == 0)
        return true;

    if (wayl->pointer.theme != NULL) {
        wl_cursor_theme_destroy(wayl->pointer.theme);
        wayl->pointer.theme = NULL;
        wayl->pointer.cursor = NULL;
    }

    LOG_DBG("reloading cursor theme: %s@%d",
            wayl->pointer.theme_name, wayl->pointer.size);

    wayl->pointer.theme = wl_cursor_theme_load(
        wayl->pointer.theme_name, wayl->pointer.size * term->scale, wayl->shm);

    if (wayl->pointer.theme == NULL) {
        LOG_ERR("failed to load cursor theme");
        return false;
    }

    return wayl_cursor_set(wayl, term);
}

struct terminal *
wayl_terminal_from_surface(struct wayland *wayl, struct wl_surface *surface)
{
    tll_foreach(wayl->terms, it) {
        if (it->item->window->surface == surface ||
            it->item->window->search_surface == surface)
        {
            return it->item;
        }
    }

    assert(false);
    LOG_WARN("surface %p doesn't map to a terminal", surface);
    return NULL;
}

struct terminal *
wayl_terminal_from_xdg_toplevel(struct wayland *wayl,
                                struct xdg_toplevel *toplevel)
{
    tll_foreach(wayl->terms, it) {
        if (it->item->window->xdg_toplevel == toplevel)
            return it->item;
    }

    assert(false);
    return NULL;
}
