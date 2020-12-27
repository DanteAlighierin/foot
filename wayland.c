#include "wayland.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>

#include <sys/timerfd.h>
#include <sys/epoll.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-shell.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <tllist.h>
#include <xdg-output-unstable-v1.h>
#include <xdg-decoration-unstable-v1.h>
#include <text-input-unstable-v3.h>

#define LOG_MODULE "wayland"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "config.h"
#include "terminal.h"
#include "ime.h"
#include "input.h"
#include "render.h"
#include "selection.h"
#include "util.h"
#include "xmalloc.h"

static void
csd_instantiate(struct wl_window *win)
{
    struct wayland *wayl = win->term->wl;
    assert(wayl != NULL);

    for (size_t i = 0; i < ALEN(win->csd.surface); i++) {
        assert(win->csd.surface[i] == NULL);
        assert(win->csd.sub_surface[i] == NULL);

        win->csd.surface[i] = wl_compositor_create_surface(wayl->compositor);

        struct wl_surface *parent = i < CSD_SURF_MINIMIZE
            ? win->surface : win->csd.surface[CSD_SURF_TITLE];

        win->csd.sub_surface[i] = wl_subcompositor_get_subsurface(
            wayl->sub_compositor, win->csd.surface[i], parent);

        wl_subsurface_set_sync(win->csd.sub_surface[i]);
        wl_surface_set_user_data(win->csd.surface[i], win);
        wl_surface_commit(win->csd.surface[i]);
    }
}

static void
csd_destroy(struct wl_window *win)
{
    for (size_t i = 0; i < ALEN(win->csd.surface); i++) {
        if (win->csd.sub_surface[i] != NULL)
            wl_subsurface_destroy(win->csd.sub_surface[i]);
        if (win->csd.surface[i] != NULL)
            wl_surface_destroy(win->csd.surface[i]);

        win->csd.surface[i] = NULL;
        win->csd.sub_surface[i] = NULL;
    }
}

static void
seat_add_data_device(struct seat *seat)
{
    if (seat->wayl->data_device_manager == NULL)
        return;

    if (seat->data_device != NULL) {
        /* TODO: destroy old device + clipboard data? */
        return;
    }

    struct wl_data_device *data_device = wl_data_device_manager_get_data_device(
        seat->wayl->data_device_manager, seat->wl_seat);

    if (data_device == NULL)
        return;

    seat->data_device = data_device;
    wl_data_device_add_listener(data_device, &data_device_listener, seat);
}

static void
seat_add_primary_selection(struct seat *seat)
{
    if (seat->wayl->primary_selection_device_manager == NULL)
        return;

    if (seat->primary_selection_device != NULL)
        return;

    struct zwp_primary_selection_device_v1 *primary_selection_device
        = zwp_primary_selection_device_manager_v1_get_device(
            seat->wayl->primary_selection_device_manager, seat->wl_seat);

    if (primary_selection_device == NULL)
        return;

    seat->primary_selection_device = primary_selection_device;
    zwp_primary_selection_device_v1_add_listener(
        primary_selection_device, &primary_selection_device_listener, seat);
}

static void
seat_add_text_input(struct seat *seat)
{
#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (seat->wayl->text_input_manager == NULL)
        return;

    struct zwp_text_input_v3 *text_input
        = zwp_text_input_manager_v3_get_text_input(
            seat->wayl->text_input_manager, seat->wl_seat);

    if (text_input == NULL)
        return;

    seat->wl_text_input = text_input;
    zwp_text_input_v3_add_listener(text_input, &text_input_listener, seat);
#endif
}

static void
seat_destroy(struct seat *seat)
{
    if (seat == NULL)
        return;

    tll_free(seat->mouse.buttons);

    tll_foreach(seat->kbd.bindings.key, it)
        tll_free(it->item.bind.key_codes);
    tll_free(seat->kbd.bindings.key);

    tll_foreach(seat->kbd.bindings.search, it)
        tll_free(it->item.bind.key_codes);
    tll_free(seat->kbd.bindings.search);

    tll_free(seat->mouse.bindings);

    if (seat->kbd.xkb_compose_state != NULL)
        xkb_compose_state_unref(seat->kbd.xkb_compose_state);
    if (seat->kbd.xkb_compose_table != NULL)
        xkb_compose_table_unref(seat->kbd.xkb_compose_table);
    if (seat->kbd.xkb_keymap != NULL)
        xkb_keymap_unref(seat->kbd.xkb_keymap);
    if (seat->kbd.xkb_state != NULL)
        xkb_state_unref(seat->kbd.xkb_state);
    if (seat->kbd.xkb != NULL)
        xkb_context_unref(seat->kbd.xkb);

    if (seat->kbd.repeat.fd >= 0)
        fdm_del(seat->wayl->fdm, seat->kbd.repeat.fd);

    if (seat->pointer.theme != NULL)
        wl_cursor_theme_destroy(seat->pointer.theme);
    if (seat->pointer.surface != NULL)
        wl_surface_destroy(seat->pointer.surface);
    if (seat->pointer.xcursor_callback != NULL)
        wl_callback_destroy(seat->pointer.xcursor_callback);

    if (seat->clipboard.data_source != NULL)
        wl_data_source_destroy(seat->clipboard.data_source);
    if (seat->clipboard.data_offer != NULL)
        wl_data_offer_destroy(seat->clipboard.data_offer);
    if (seat->primary.data_source != NULL)
        zwp_primary_selection_source_v1_destroy(seat->primary.data_source);
    if (seat->primary.data_offer != NULL)
        zwp_primary_selection_offer_v1_destroy(seat->primary.data_offer);
    if (seat->primary_selection_device != NULL)
        zwp_primary_selection_device_v1_destroy(seat->primary_selection_device);
    if (seat->data_device != NULL)
        wl_data_device_release(seat->data_device);

    if (seat->wl_keyboard != NULL)
        wl_keyboard_release(seat->wl_keyboard);
    if (seat->wl_pointer != NULL)
        wl_pointer_release(seat->wl_pointer);

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (seat->wl_text_input != NULL)
        zwp_text_input_v3_destroy(seat->wl_text_input);
#endif

    if (seat->wl_seat != NULL)
        wl_seat_release(seat->wl_seat);

    ime_reset(seat);
    free(seat->clipboard.text);
    free(seat->primary.text);
    free(seat->name);
}

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
    struct seat *seat = data;
    assert(seat->wl_seat == wl_seat);

    LOG_DBG("%s: keyboard=%s, pointer=%s", seat->name,
            (caps & WL_SEAT_CAPABILITY_KEYBOARD) ? "yes" : "no",
            (caps & WL_SEAT_CAPABILITY_POINTER) ? "yes" : "no");

    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (seat->wl_keyboard == NULL) {
            seat->wl_keyboard = wl_seat_get_keyboard(wl_seat);
            wl_keyboard_add_listener(seat->wl_keyboard, &keyboard_listener, seat);
        }
    } else {
        if (seat->wl_keyboard != NULL) {
            wl_keyboard_release(seat->wl_keyboard);
            seat->wl_keyboard = NULL;
        }
    }

    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        if (seat->wl_pointer == NULL) {
            assert(seat->pointer.surface == NULL);
            seat->pointer.surface = wl_compositor_create_surface(seat->wayl->compositor);

            if (seat->pointer.surface == NULL) {
                LOG_ERR("%s: failed to create pointer surface", seat->name);
                return;
            }

            seat->wl_pointer = wl_seat_get_pointer(wl_seat);
            wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);
        }
    } else {
        if (seat->wl_pointer != NULL) {
            wl_pointer_release(seat->wl_pointer);
            wl_surface_destroy(seat->pointer.surface);

            if (seat->pointer.theme != NULL)
                wl_cursor_theme_destroy(seat->pointer.theme);

            seat->wl_pointer = NULL;
            seat->pointer.surface = NULL;
            seat->pointer.theme = NULL;
            seat->pointer.cursor = NULL;
        }
    }
}

static void
seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
    struct seat *seat = data;
    free(seat->name);
    seat->name = xstrdup(name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

static void
update_term_for_output_change(struct terminal *term)
{
    if (tll_length(term->window->on_outputs) == 0)
        return;

    render_resize(term, term->width / term->scale, term->height / term->scale);
    term_font_dpi_changed(term);
    term_font_subpixel_changed(term);
}

static void
update_terms_on_monitor(struct monitor *mon)
{
    struct wayland *wayl = mon->wayl;

    tll_foreach(wayl->terms, it) {
        struct terminal *term = it->item;

        tll_foreach(term->window->on_outputs, it2) {
            if (it2->item == mon) {
                update_term_for_output_change(term);
                break;
            }
        }
    }
}

static void
output_update_ppi(struct monitor *mon)
{
    if (mon->dim.mm.width == 0 || mon->dim.mm.height == 0)
        return;

    int x_inches = mon->dim.mm.width * 0.03937008;
    int y_inches = mon->dim.mm.height * 0.03937008;

    mon->ppi.real.x = mon->dim.px_real.width / x_inches;
    mon->ppi.real.y = mon->dim.px_real.height / y_inches;

    mon->ppi.scaled.x = mon->dim.px_scaled.width / x_inches;
    mon->ppi.scaled.y = mon->dim.px_scaled.height / y_inches;

    float px_diag = sqrt(
        pow(mon->dim.px_scaled.width, 2) +
        pow(mon->dim.px_scaled.height, 2));

    mon->dpi = px_diag / mon->inch * mon->scale;
}

static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                int32_t physical_width, int32_t physical_height,
                int32_t subpixel, const char *make, const char *model,
                int32_t transform)
{
    struct monitor *mon = data;
    mon->dim.mm.width = physical_width;
    mon->dim.mm.height = physical_height;
    mon->inch = sqrt(pow(mon->dim.mm.width, 2) + pow(mon->dim.mm.height, 2)) * 0.03937008;
    mon->make = make != NULL ? xstrdup(make) : NULL;
    mon->model = model != NULL ? xstrdup(model) : NULL;
    mon->subpixel = subpixel;
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
    mon->dim.px_real.width = width;
    mon->dim.px_real.height = height;
    output_update_ppi(mon);
}

static void
output_done(void *data, struct wl_output *wl_output)
{
    struct monitor *mon = data;
    update_terms_on_monitor(mon);
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    struct monitor *mon = data;
    mon->scale = factor;
    output_update_ppi(mon);
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
    mon->dim.px_scaled.width = width;
    mon->dim.px_scaled.height = height;
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
    mon->name = name != NULL ? xstrdup(name) : NULL;
}

static void
xdg_output_handle_description(void *data, struct zxdg_output_v1 *xdg_output,
                              const char *description)
{
    struct monitor *mon = data;
    mon->description = description != NULL ? xstrdup(description) : NULL;
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
surface_enter(void *data, struct wl_surface *wl_surface,
              struct wl_output *wl_output)
{
    struct wl_window *win = data;
    struct terminal *term = win->term;

    tll_foreach(term->wl->monitors, it) {
        if (it->item.output == wl_output) {
            LOG_DBG("mapped on %s", it->item.name);
            tll_push_back(term->window->on_outputs, &it->item);
            update_term_for_output_change(term);
            return;
        }
    }

    LOG_ERR("mapped on unknown output");
}

static void
surface_leave(void *data, struct wl_surface *wl_surface,
              struct wl_output *wl_output)
{
    struct wl_window *win = data;
    struct terminal *term = win->term;

    tll_foreach(term->window->on_outputs, it) {
        if (it->item->output != wl_output)
            continue;

        LOG_DBG("unmapped from %s", it->item->name);
        tll_remove(term->window->on_outputs, it);
        update_term_for_output_change(term);
        return;
    }

    LOG_WARN("unmapped from unknown output");
}

static const struct wl_surface_listener surface_listener = {
    .enter = &surface_enter,
    .leave = &surface_leave,
};

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                       int32_t width, int32_t height, struct wl_array *states)
{
    bool is_activated = false;
    bool is_fullscreen = false;
    bool is_maximized = false;
    bool is_tiled_top = false;
    bool is_tiled_bottom = false;
    bool is_tiled_left = false;
    bool is_tiled_right = false;

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    char state_str[2048];
    int state_chars = 0;

    static const char *const strings[] = {
        [XDG_TOPLEVEL_STATE_MAXIMIZED] = "maximized",
        [XDG_TOPLEVEL_STATE_FULLSCREEN] = "fullscreen",
        [XDG_TOPLEVEL_STATE_RESIZING] = "resizing",
        [XDG_TOPLEVEL_STATE_ACTIVATED] = "activated",
        [XDG_TOPLEVEL_STATE_TILED_LEFT] = "tiled:left",
        [XDG_TOPLEVEL_STATE_TILED_RIGHT] = "tiled:right",
        [XDG_TOPLEVEL_STATE_TILED_TOP] = "tiled:top",
        [XDG_TOPLEVEL_STATE_TILED_BOTTOM] = "tiled:bottom",
    };
#endif

    enum xdg_toplevel_state *state;
    wl_array_for_each(state, states) {
        switch (*state) {
        case XDG_TOPLEVEL_STATE_ACTIVATED:    is_activated = true; break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:   is_fullscreen = true; break;
        case XDG_TOPLEVEL_STATE_MAXIMIZED:    is_maximized = true; break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:   is_tiled_left = true; break;
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:  is_tiled_right = true; break;
        case XDG_TOPLEVEL_STATE_TILED_TOP:    is_tiled_top = true; break;
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM: is_tiled_bottom = true; break;

        case XDG_TOPLEVEL_STATE_RESIZING:
            /* Ignored */
            /* TODO: throttle? */
            break;
        }

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
        if (*state >= XDG_TOPLEVEL_STATE_MAXIMIZED &&
            *state <= XDG_TOPLEVEL_STATE_TILED_BOTTOM)
        {
            state_chars += snprintf(
                &state_str[state_chars], sizeof(state_str) - state_chars,
                "%s, ", strings[*state]);
        }
#endif
    }

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    if (state_chars > 2)
        state_str[state_chars - 2] = '\0';
    else
        state_str[0] = '\0';

    LOG_DBG("xdg-toplevel: configure: size=%dx%d, states=[%s]",
            width, height, state_str);
#endif

    /*
     * Changes done here are ignored until the configure event has
     * been ack:ed in xdg_surface_configure().
     *
     * So, just store the config data and apply it later, in
     * xdg_surface_configure() after we've ack:ed the event.
     */
    struct wl_window *win = data;

    if (!is_fullscreen && win->use_csd == CSD_YES && width > 0 && height > 0) {
        /*
         * We include the CSD title bar in our window geometry. Thus,
         * the height we call render_resize() with must be adjusted,
         * since it expects the size to refer to the main grid only.
         */
        height -= win->term->conf->csd.title_height;
    }
    win->configure.is_activated = is_activated;
    win->configure.is_fullscreen = is_fullscreen;
    win->configure.is_maximized = is_maximized;
    win->configure.is_tiled_top = is_tiled_top;
    win->configure.is_tiled_bottom = is_tiled_bottom;
    win->configure.is_tiled_left = is_tiled_left;
    win->configure.is_tiled_right = is_tiled_right;
    win->configure.width = width;
    win->configure.height = height;
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    struct wl_window *win = data;
    struct terminal *term = win->term;
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
    LOG_DBG("xdg-surface: configure");

    struct wl_window *win = data;
    struct terminal *term = win->term;

    bool wasnt_configured = !win->is_configured;
    win->is_configured = true;
    win->is_maximized = win->configure.is_maximized;
    win->is_tiled_top = win->configure.is_tiled_top;
    win->is_tiled_bottom = win->configure.is_tiled_bottom;
    win->is_tiled_left = win->configure.is_tiled_left;
    win->is_tiled_right = win->configure.is_tiled_right;
    win->is_tiled = win->is_tiled_top || win->is_tiled_bottom || win->is_tiled_left || win->is_tiled_right;

    if (win->is_fullscreen != win->configure.is_fullscreen && win->use_csd == CSD_YES) {
        if (win->configure.is_fullscreen)
            csd_destroy(win);
        else
            csd_instantiate(win);
    }
    win->is_fullscreen = win->configure.is_fullscreen;

    xdg_surface_ack_configure(xdg_surface, serial);

    if (term->window->frame_callback != NULL) {
        /*
         * Preempt render scheduling.
         *
         * Each configure event require a corresponding new
         * surface+commit. Thus we cannot just schedule a pending
         * refresh if there’s already a frame being rendered.
         */
        wl_callback_destroy(term->window->frame_callback);
        term->window->frame_callback = NULL;
    }

    bool resized = render_resize(
        term, win->configure.width, win->configure.height);

    if (win->configure.is_activated)
        term_visual_focus_in(term);
    else
        term_visual_focus_out(term);

    if (!resized) {
        /*
         * If we didn't resize, we won't be committing a new surface
         * anytime soon. Some compositors require a commit in
         * combination with an ack - make them happy.
         */
        wl_surface_commit(win->surface);
    }

    if (wasnt_configured)
        term_window_configured(term);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = &xdg_surface_configure,
};

static void
xdg_toplevel_decoration_configure(void *data,
                                  struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1,
                                  uint32_t mode)
{
    struct wl_window *win = data;

    assert(win->term->conf->csd.preferred != CONF_CSD_PREFER_NONE);
    switch (mode) {
    case ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
        LOG_INFO("using CSD decorations");
        win->use_csd = CSD_YES;
        csd_instantiate(win);
        break;

    case ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
        LOG_INFO("using SSD decorations");
        win->use_csd = CSD_NO;
        csd_destroy(win);
        break;

    default:
        LOG_ERR("unimplemented: unknown XDG toplevel decoration mode: %u", mode);
        break;
    }

    if (win->is_configured && win->use_csd == CSD_YES) {
        struct terminal *term = win->term;

        int scale = term->scale;
        int width = term->width / scale;
        int height = term->height / scale;

        /* Take CSD title bar into account */
        height -= term->conf->csd.title_height;
        render_resize_force(term, width, height);
    }
}

static const struct zxdg_toplevel_decoration_v1_listener xdg_toplevel_decoration_listener = {
    .configure = &xdg_toplevel_decoration_configure,
};

static bool
fdm_repeat(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct seat *seat = data;
    uint64_t expiration_count;
    ssize_t ret = read(
        seat->kbd.repeat.fd, &expiration_count, sizeof(expiration_count));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read repeat key from repeat timer fd");
        return false;
    }

    seat->kbd.repeat.dont_re_repeat = true;
    for (size_t i = 0; i < expiration_count; i++)
        input_repeat(seat, seat->kbd.repeat.key);
    seat->kbd.repeat.dont_re_repeat = false;
    return true;
}

static void
handle_global(void *data, struct wl_registry *registry,
              uint32_t name, const char *interface, uint32_t version)
{
    LOG_DBG("global: 0x%08x, interface=%s, version=%u", name, interface, version);
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
    }

    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        /*
         * We *require* version 1, but _can_ use version 2. Version 2
         * adds 'tiled' window states. We use that information to
         * restore the window size when window is un-tiled.
         */

        wayl->shell = wl_registry_bind(
            wayl->registry, name, &xdg_wm_base_interface, min(version, 2));
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

        int repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (repeat_fd == -1) {
            LOG_ERRNO("failed to create keyboard repeat timer FD");
            return;
        }

        struct wl_seat *wl_seat = wl_registry_bind(
            wayl->registry, name, &wl_seat_interface, required);

        tll_push_back(wayl->seats, ((struct seat){
                    .wayl = wayl,
                    .wl_seat = wl_seat,
                    .wl_name = name,
                    .kbd = {
                        .repeat = {
                            .fd = repeat_fd,
                        },
                    }}));

        struct seat *seat = &tll_back(wayl->seats);

        if (!fdm_add(wayl->fdm, repeat_fd, EPOLLIN, &fdm_repeat, seat)) {
            close(repeat_fd);
            seat->kbd.repeat.fd = -1;
            seat_destroy(seat);
            return;
        }

        seat_add_data_device(seat);
        seat_add_primary_selection(seat);
        seat_add_text_input(seat);
        wl_seat_add_listener(wl_seat, &seat_listener, seat);
    }

    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->xdg_output_manager = wl_registry_bind(
            wayl->registry, name, &zxdg_output_manager_v1_interface,
            min(version, 2));

        tll_foreach(wayl->monitors, it) {
            struct monitor *mon = &it->item;
            mon->xdg = zxdg_output_manager_v1_get_xdg_output(
                wayl->xdg_output_manager, mon->output);
            zxdg_output_v1_add_listener(mon->xdg, &xdg_output_listener, mon);
        }
    }

    else if (strcmp(interface, wl_output_interface.name) == 0) {
        const uint32_t required = 2;
        if (!verify_iface_version(interface, version, required))
            return;

        struct wl_output *output = wl_registry_bind(
            wayl->registry, name, &wl_output_interface, min(version, 3));

        tll_push_back(
            wayl->monitors,
            ((struct monitor){.wayl = wayl, .output = output, .wl_name = name,
             .use_output_release = version >= WL_OUTPUT_RELEASE_SINCE_VERSION}));

        struct monitor *mon = &tll_back(wayl->monitors);
        wl_output_add_listener(output, &output_listener, mon);

        if (wayl->xdg_output_manager != NULL) {
            mon->xdg = zxdg_output_manager_v1_get_xdg_output(
                wayl->xdg_output_manager, mon->output);
            zxdg_output_v1_add_listener(mon->xdg, &xdg_output_listener, mon);
        }
    }

    else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        const uint32_t required = 3;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->data_device_manager = wl_registry_bind(
            wayl->registry, name, &wl_data_device_manager_interface, required);

        tll_foreach(wayl->seats, it)
            seat_add_data_device(&it->item);
    }

    else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->primary_selection_device_manager = wl_registry_bind(
            wayl->registry, name,
            &zwp_primary_selection_device_manager_v1_interface, required);

        tll_foreach(wayl->seats, it)
            seat_add_primary_selection(&it->item);
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

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->text_input_manager = wl_registry_bind(
            wayl->registry, name, &zwp_text_input_manager_v3_interface, required);

        tll_foreach(wayl->seats, it)
            seat_add_text_input(&it->item);
    }
#endif
}

static void
monitor_destroy(struct monitor *mon)
{
    if (mon->xdg != NULL)
        zxdg_output_v1_destroy(mon->xdg);
    if (mon->output != NULL) {
        if (mon->use_output_release)
            wl_output_release(mon->output);
        else
            wl_output_destroy(mon->output);
    }
    free(mon->make);
    free(mon->model);
    free(mon->name);
    free(mon->description);
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    LOG_DBG("global removed: 0x%08x", name);

    struct wayland *wayl = data;

    /* Check if this is an output */
    tll_foreach(wayl->monitors, it) {
        struct monitor *mon = &it->item;

        if (mon->wl_name != name)
            continue;

        LOG_INFO("monitor unplugged or disabled: %s", mon->name);

        /*
         * Update all terminals that are mapped here. On Sway 1.4,
         * surfaces are *not* unmapped before the output is removed
         */

        tll_foreach(wayl->terms, t) {
            tll_foreach(t->item->window->on_outputs, o) {
                if (o->item->output == mon->output) {
                    surface_leave(t->item->window, NULL, mon->output);
                    break;
                }
            }
        }

        monitor_destroy(mon);
        tll_remove(wayl->monitors, it);
        return;
    }

    /* A seat? */
    tll_foreach(wayl->seats, it) {
        struct seat *seat = &it->item;

        if (seat->wl_name != name)
            continue;

        LOG_INFO("seat destroyed: %s", seat->name);

        if (seat->kbd_focus != NULL) {
            LOG_WARN("compositor destroyed seat '%s' "
                     "without sending a keyboard leave event",
                     seat->name);

            if (seat->wl_keyboard != NULL)
                keyboard_listener.leave(
                    seat, seat->wl_keyboard, -1, seat->kbd_focus->window->surface);
        }

        if (seat->mouse_focus != NULL) {
            LOG_WARN("compositor destroyed seat '%s' "
                     "without sending a pointer leave event",
                     seat->name);

            if (seat->wl_pointer != NULL)
                pointer_listener.leave(
                    seat, seat->wl_pointer, -1, seat->mouse_focus->window->surface);
        }

        seat_destroy(seat);
        tll_remove(wayl->seats, it);
        return;
    }

    LOG_WARN("unknown global removed: 0x%08x", name);
}

static const struct wl_registry_listener registry_listener = {
    .global = &handle_global,
    .global_remove = &handle_global_remove,
};

static void
fdm_hook(struct fdm *fdm, void *data)
{
    struct wayland *wayl = data;
    wayl_flush(wayl);
}

static bool
fdm_wayl(struct fdm *fdm, int fd, int events, void *data)
{
    struct wayland *wayl = data;
    int event_count = 0;

    if (events & EPOLLIN) {
        if (wl_display_read_events(wayl->display) < 0) {
            LOG_ERRNO("failed to read events from the Wayland socket");
            return false;
        }

        while (wl_display_prepare_read(wayl->display) != 0)
            wl_display_dispatch_pending(wayl->display);
    }

    if (events & EPOLLHUP) {
        LOG_WARN("disconnected from Wayland");
        wl_display_cancel_read(wayl->display);
        return false;
    }

    return event_count != -1;
}

struct wayland *
wayl_init(const struct config *conf, struct fdm *fdm)
{
    struct wayland *wayl = calloc(1, sizeof(*wayl));
    if (unlikely(wayl == NULL)) {
        LOG_ERRNO("calloc() failed");
        return NULL;
    }

    wayl->conf = conf;
    wayl->fdm = fdm;
    wayl->fd = -1;

    if (!fdm_hook_add(fdm, &fdm_hook, wayl, FDM_HOOK_PRIORITY_LOW)) {
        LOG_ERR("failed to add FDM hook");
        goto out;
    }

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
    if (wayl->data_device_manager == NULL) {
        LOG_ERR("no clipboard available "
                "(wl_data_device_manager not implemented by server)");
        goto out;
    }
    if (wayl->primary_selection_device_manager == NULL)
        LOG_WARN("no primary selection available");

    if (conf->presentation_timings && wayl->presentation == NULL) {
        LOG_ERR("presentation time interface not implemented by compositor");
        goto out;
    }

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (wayl->text_input_manager == NULL) {
        LOG_WARN("text input interface not implemented by compositor; "
                 "IME will be disabled");
    }
#endif

    /* Trigger listeners registered when handling globals */
    wl_display_roundtrip(wayl->display);

    if (!wayl->have_argb8888) {
        LOG_ERR("compositor does not support ARGB surfaces");
        goto out;
    }

    tll_foreach(wayl->monitors, it) {
        LOG_INFO(
            "%s: %dx%d+%dx%d@%dHz %s %.2f\" scale=%d PPI=%dx%d (physical) PPI=%dx%d (logical), DPI=%.2f",
            it->item.name, it->item.dim.px_real.width, it->item.dim.px_real.height,
            it->item.x, it->item.y, (int)round(it->item.refresh),
            it->item.model != NULL ? it->item.model : it->item.description,
            it->item.inch, it->item.scale,
            it->item.ppi.real.x, it->item.ppi.real.y,
            it->item.ppi.scaled.x, it->item.ppi.scaled.y,
            it->item.dpi);
    }

    wayl->fd = wl_display_get_fd(wayl->display);
    if (fcntl(wayl->fd, F_SETFL, fcntl(wayl->fd, F_GETFL) | O_NONBLOCK) < 0) {
        LOG_ERRNO("failed to make Wayland socket non-blocking");
        goto out;
    }

    if (!fdm_add(fdm, wayl->fd, EPOLLIN, &fdm_wayl, wayl))
        goto out;

    if (wl_display_prepare_read(wayl->display) != 0) {
        LOG_ERRNO("failed to prepare for reading wayland events");
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

    fdm_hook_del(wayl->fdm, &fdm_hook, FDM_HOOK_PRIORITY_LOW);

    tll_foreach(wayl->monitors, it)
        monitor_destroy(&it->item);
    tll_free(wayl->monitors);

    tll_foreach(wayl->seats, it)
        seat_destroy(&it->item);
    tll_free(wayl->seats);

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (wayl->text_input_manager != NULL)
        zwp_text_input_manager_v3_destroy(wayl->text_input_manager);
#endif

    if (wayl->xdg_output_manager != NULL)
        zxdg_output_manager_v1_destroy(wayl->xdg_output_manager);
    if (wayl->shell != NULL)
        xdg_wm_base_destroy(wayl->shell);
    if (wayl->xdg_decoration_manager != NULL)
        zxdg_decoration_manager_v1_destroy(wayl->xdg_decoration_manager);
    if (wayl->presentation != NULL)
        wp_presentation_destroy(wayl->presentation);
    if (wayl->data_device_manager != NULL)
        wl_data_device_manager_destroy(wayl->data_device_manager);
    if (wayl->primary_selection_device_manager != NULL)
        zwp_primary_selection_device_manager_v1_destroy(wayl->primary_selection_device_manager);
    if (wayl->shm != NULL)
        wl_shm_destroy(wayl->shm);
    if (wayl->sub_compositor != NULL)
        wl_subcompositor_destroy(wayl->sub_compositor);
    if (wayl->compositor != NULL)
        wl_compositor_destroy(wayl->compositor);
    if (wayl->registry != NULL)
        wl_registry_destroy(wayl->registry);
    if (wayl->fd != -1)
        fdm_del_no_close(wayl->fdm, wayl->fd);
    if (wayl->display != NULL) {
        wayl_flush(wayl);
        wl_display_disconnect(wayl->display);
    }

    free(wayl);
}

struct wl_window *
wayl_win_init(struct terminal *term)
{
    struct wayland *wayl = term->wl;
    const struct config *conf = term->conf;

    struct wl_window *win = calloc(1, sizeof(*win));
    if (unlikely(win == NULL)) {
        LOG_ERRNO("calloc() failed");
        return NULL;
    }

    win->term = term;
    win->use_csd = CSD_UNKNOWN;
    win->csd.move_timeout_fd = -1;

    win->surface = wl_compositor_create_surface(wayl->compositor);
    if (win->surface == NULL) {
        LOG_ERR("failed to create wayland surface");
        goto out;
    }

    if (term->colors.alpha == 0xffff) {
        struct wl_region *region = wl_compositor_create_region(
            term->wl->compositor);

        if (region != NULL) {
            wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
            wl_surface_set_opaque_region(win->surface, region);
            wl_region_destroy(region);
        }
    }

    wl_surface_add_listener(win->surface, &surface_listener, win);

    win->xdg_surface = xdg_wm_base_get_xdg_surface(wayl->shell, win->surface);
    xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, win);

    win->xdg_toplevel = xdg_surface_get_toplevel(win->xdg_surface);
    xdg_toplevel_add_listener(win->xdg_toplevel, &xdg_toplevel_listener, win);

    xdg_toplevel_set_app_id(win->xdg_toplevel, conf->app_id);

    if (conf->csd.preferred == CONF_CSD_PREFER_NONE) {
        /* User specifically do *not* want decorations */
        win->use_csd = CSD_NO;
        LOG_INFO("window decorations disabled by user");
    } else if (wayl->xdg_decoration_manager != NULL) {
        win->xdg_toplevel_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            wayl->xdg_decoration_manager, win->xdg_toplevel);

        LOG_INFO("requesting %s decorations",
                 conf->csd.preferred == CONF_CSD_PREFER_SERVER ? "SSD" : "CSD");

        zxdg_toplevel_decoration_v1_set_mode(
            win->xdg_toplevel_decoration,
            (conf->csd.preferred == CONF_CSD_PREFER_SERVER
             ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
             : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE));

        zxdg_toplevel_decoration_v1_add_listener(
            win->xdg_toplevel_decoration, &xdg_toplevel_decoration_listener, win);
    } else {
        /* No decoration manager - thus we *must* draw our own decorations */
        win->use_csd = CSD_YES;
        csd_instantiate(win);
        LOG_WARN("no decoration manager available - using CSDs unconditionally");
    }

    wl_surface_commit(win->surface);

    if (conf->tweak.render_timer_osd) {
        win->render_timer_surface = wl_compositor_create_surface(wayl->compositor);
        if (win->render_timer_surface == NULL) {
            LOG_ERR("failed to create render timer surface");
            goto out;
        }
        win->render_timer_sub_surface = wl_subcompositor_get_subsurface(
            wayl->sub_compositor, win->render_timer_surface, win->surface);
        wl_subsurface_set_sync(win->render_timer_sub_surface);
        wl_surface_set_user_data(win->render_timer_surface, win);
        wl_surface_commit(win->render_timer_surface);
    }
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

    if (win->csd.move_timeout_fd != -1)
        close(win->csd.move_timeout_fd);

    /*
     * First, unmap all surfaces to trigger things like
     * keyboard_leave() and wl_pointer_leave().
     *
     * This ensures we remove all references to *this* window from the
     * global wayland struct (since it no longer has neither keyboard
     * nor mouse focus).
     */

    if (win->render_timer_surface != NULL) {
        wl_surface_attach(win->render_timer_surface, NULL, 0, 0);
        wl_surface_commit(win->render_timer_surface);
    }

    if (win->scrollback_indicator_surface != NULL) {
        wl_surface_attach(win->scrollback_indicator_surface, NULL, 0, 0);
        wl_surface_commit(win->scrollback_indicator_surface);
    }

    /* Scrollback search */
    if (win->search_surface != NULL) {
        wl_surface_attach(win->search_surface, NULL, 0, 0);
        wl_surface_commit(win->search_surface);
    }

    /* CSD */
    for (size_t i = 0; i < ALEN(win->csd.surface); i++) {
        if (win->csd.surface[i] != NULL) {
            wl_surface_attach(win->csd.surface[i], NULL, 0, 0);
            wl_surface_commit(win->csd.surface[i]);
        }
    }

    wayl_roundtrip(win->term->wl);

        /* Main window */
    wl_surface_attach(win->surface, NULL, 0, 0);
    wl_surface_commit(win->surface);
    wayl_roundtrip(win->term->wl);

    tll_free(win->on_outputs);

    csd_destroy(win);
    if (win->render_timer_sub_surface != NULL)
        wl_subsurface_destroy(win->render_timer_sub_surface);
    if (win->render_timer_surface != NULL)
        wl_surface_destroy(win->render_timer_surface);
    if (win->scrollback_indicator_sub_surface != NULL)
        wl_subsurface_destroy(win->scrollback_indicator_sub_surface);
    if (win->scrollback_indicator_surface != NULL)
        wl_surface_destroy(win->scrollback_indicator_surface);
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

    wayl_roundtrip(win->term->wl);
    free(win);
}

bool
wayl_reload_xcursor_theme(struct seat *seat, int new_scale)
{
    if (seat->pointer.theme != NULL && seat->pointer.scale == new_scale) {
        /* We already have a theme loaded, and the scale hasn't changed */
        return true;
    }

    if (seat->pointer.theme != NULL) {
        assert(seat->pointer.scale != new_scale);
        wl_cursor_theme_destroy(seat->pointer.theme);
        seat->pointer.theme = NULL;
        seat->pointer.cursor = NULL;
    }

    const char *xcursor_theme = getenv("XCURSOR_THEME");
    int xcursor_size = 24;

    {
        const char *env_cursor_size = getenv("XCURSOR_SIZE");
        if (env_cursor_size != NULL) {
            unsigned size;
            if (sscanf(env_cursor_size, "%u", &size) == 1)
                xcursor_size = size;
        }
    }

    LOG_INFO("cursor theme: %s, size: %u, scale: %d",
             xcursor_theme, xcursor_size, new_scale);

    seat->pointer.theme = wl_cursor_theme_load(
        xcursor_theme, xcursor_size * new_scale, seat->wayl->shm);

    if (seat->pointer.theme == NULL) {
        LOG_ERR("failed to load cursor theme");
        return false;
    }

    seat->pointer.scale = new_scale;
    return true;
}

void
wayl_flush(struct wayland *wayl)
{
    while (true) {
        int r = wl_display_flush(wayl->display);
        if (r >= 0) {
            /* Most likely code path - the flush succeed */
            return;
        }

        if (errno == EINTR) {
            /* Unlikely */
            continue;
        }

        if (errno != EAGAIN) {
            LOG_ERRNO("failed to flush wayland socket");
            return;
        }

        /* Socket buffer is full - need to wait for it to become
           writeable again */
        assert(errno == EAGAIN);

        while (true) {
            struct pollfd fds[] = {{.fd = wayl->fd, .events = POLLOUT}};

            r = poll(fds, sizeof(fds) / sizeof(fds[0]), -1);

            if (r < 0) {
                if (errno == EINTR)
                    continue;

                LOG_ERRNO("failed to poll");
                return;
            }

            if (fds[0].revents & POLLHUP)
                return;

            assert(fds[0].revents & POLLOUT);
            break;
        }
    }
}

void
wayl_roundtrip(struct wayland *wayl)
{
    wl_display_cancel_read(wayl->display);
    wl_display_roundtrip(wayl->display);

    /* I suspect the roundtrip above clears the pending queue, and
     * that prepare_read() will always succeed in the first call. But,
     * better safe than sorry... */

    while (wl_display_prepare_read(wayl->display) != 0)
        wl_display_dispatch_pending(wayl->display);
    wayl_flush(wayl);
}
