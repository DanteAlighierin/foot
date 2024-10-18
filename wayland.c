#include "wayland.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/timerfd.h>
#include <sys/epoll.h>

#include <cursor-shape-v1.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <tllist.h>

#define LOG_MODULE "wayland"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "config.h"
#include "terminal.h"
#include "ime.h"
#include "input.h"
#include "render.h"
#include "selection.h"
#include "shm.h"
#include "shm-formats.h"
#include "util.h"
#include "xmalloc.h"

static void
csd_reload_font(struct wl_window *win, float old_scale)
{
    struct terminal *term = win->term;
    const struct config *conf = term->conf;

    const float scale = term->scale;

    bool enable_csd = win->csd_mode == CSD_YES && !win->is_fullscreen;
    if (!enable_csd)
        return;
    if (win->csd.font != NULL && scale == old_scale)
        return;

    fcft_destroy(win->csd.font);

    const char *patterns[conf->csd.font.count];
    for (size_t i = 0; i < conf->csd.font.count; i++)
        patterns[i] = conf->csd.font.arr[i].pattern;

    char pixelsize[32];
    snprintf(pixelsize, sizeof(pixelsize), "pixelsize=%u",
             (int)roundf(conf->csd.title_height * scale * 1 / 2));

    LOG_DBG("loading CSD font \"%s:%s\" (old-scale=%.2f, scale=%.2f)",
            patterns[0], pixelsize, old_scale, scale);

    win->csd.font = fcft_from_name(conf->csd.font.count, patterns, pixelsize);
}

static void
csd_instantiate(struct wl_window *win)
{
    struct wayland *wayl = win->term->wl;
    xassert(wayl != NULL);

    for (size_t i = 0; i < CSD_SURF_MINIMIZE; i++) {
        bool ret = wayl_win_subsurface_new(win, &win->csd.surface[i], true);
        xassert(ret);
    }

    for (size_t i = CSD_SURF_MINIMIZE; i < CSD_SURF_COUNT; i++) {
        bool ret = wayl_win_subsurface_new_with_custom_parent(
            win, win->csd.surface[CSD_SURF_TITLE].surface.surf, &win->csd.surface[i],
            true);
        xassert(ret);
    }

    csd_reload_font(win, -1.);
}

static void
csd_destroy(struct wl_window *win)
{
    struct terminal *term = win->term;

    fcft_destroy(term->window->csd.font);
    term->window->csd.font = NULL;

    for (size_t i = 0; i < ALEN(win->csd.surface); i++)
        wayl_win_subsurface_destroy(&win->csd.surface[i]);
    shm_purge(term->render.chains.csd);
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
seat_add_key_bindings(struct seat *seat)
{
    key_binding_new_for_seat(seat->wayl->key_binding_manager, seat);
}

static void
seat_destroy(struct seat *seat)
{
    if (seat == NULL)
        return;

    tll_free(seat->mouse.buttons);
    key_binding_remove_seat(seat->wayl->key_binding_manager, seat);

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
    if (seat->pointer.surface.surf != NULL)
        wl_surface_destroy(seat->pointer.surface.surf);
    if (seat->pointer.surface.viewport != NULL)
        wp_viewport_destroy(seat->pointer.surface.viewport);
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
    if (seat->pointer.shape_device != NULL)
        wp_cursor_shape_device_v1_destroy(seat->pointer.shape_device);
    if (seat->wl_keyboard != NULL)
        wl_keyboard_release(seat->wl_keyboard);
    if (seat->wl_pointer != NULL)
        wl_pointer_release(seat->wl_pointer);
    if (seat->wl_touch != NULL)
        wl_touch_release(seat->wl_touch);

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (seat->wl_text_input != NULL)
        zwp_text_input_v3_destroy(seat->wl_text_input);
#endif

    if (seat->wl_seat != NULL)
        wl_seat_release(seat->wl_seat);

    ime_reset_pending(seat);
    free(seat->clipboard.text);
    free(seat->primary.text);
    free(seat->pointer.last_custom_xcursor);
    free(seat->name);
}

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
#if defined(_DEBUG)
    bool have_description = false;

    for (size_t i = 0; i < ALEN(shm_formats); i++) {
        if (shm_formats[i].format == format) {
            LOG_DBG("shm: 0x%08x: %s", format, shm_formats[i].description);
            have_description = true;
            break;
        }
    }

    if (!have_description)
        LOG_DBG("shm: 0x%08x: unknown", format);
#endif
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
    xassert(seat->wl_seat == wl_seat);

    LOG_DBG("%s: keyboard=%s, pointer=%s, touch=%s", seat->name,
            (caps & WL_SEAT_CAPABILITY_KEYBOARD) ? "yes" : "no",
            (caps & WL_SEAT_CAPABILITY_POINTER) ? "yes" : "no",
            (caps & WL_SEAT_CAPABILITY_TOUCH) ? "yes" : "no");

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
            xassert(seat->pointer.surface.surf == NULL);
            seat->pointer.surface.surf =
                wl_compositor_create_surface(seat->wayl->compositor);

            if (seat->pointer.surface.surf == NULL) {
                LOG_ERR("%s: failed to create pointer surface", seat->name);
                return;
            }

            if (seat->wayl->viewporter != NULL) {
                xassert(seat->pointer.surface.viewport == NULL);
                seat->pointer.surface.viewport = wp_viewporter_get_viewport(
                    seat->wayl->viewporter, seat->pointer.surface.surf);

                if (seat->pointer.surface.viewport == NULL) {
                    LOG_ERR("%s: failed to create pointer viewport", seat->name);
                    wl_surface_destroy(seat->pointer.surface.surf);
                    seat->pointer.surface.surf = NULL;
                    return;
                }
            }

            seat->wl_pointer = wl_seat_get_pointer(wl_seat);
            wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);

            if (seat->wayl->cursor_shape_manager != NULL) {
                xassert(seat->pointer.shape_device == NULL);
                seat->pointer.shape_device = wp_cursor_shape_manager_v1_get_pointer(
                    seat->wayl->cursor_shape_manager, seat->wl_pointer);
            }
        }
    } else {
        if (seat->wl_pointer != NULL) {
            if (seat->pointer.shape_device != NULL) {
                wp_cursor_shape_device_v1_destroy(seat->pointer.shape_device);
                seat->pointer.shape_device = NULL;
            }

            wl_pointer_release(seat->wl_pointer);
            wl_surface_destroy(seat->pointer.surface.surf);

            if (seat->pointer.surface.viewport != NULL) {
                wp_viewport_destroy(seat->pointer.surface.viewport);
                seat->pointer.surface.viewport = NULL;
            }

            if (seat->pointer.theme != NULL)
                wl_cursor_theme_destroy(seat->pointer.theme);

            if (seat->wl_touch != NULL &&
                seat->touch.state == TOUCH_STATE_INHIBITED)
            {
                seat->touch.state = TOUCH_STATE_IDLE;
            }

            seat->wl_pointer = NULL;
            seat->pointer.surface.surf = NULL;
            seat->pointer.theme = NULL;
            seat->pointer.cursor = NULL;
        }
    }

    if (caps & WL_SEAT_CAPABILITY_TOUCH) {
        if (seat->wl_touch == NULL) {
            seat->wl_touch = wl_seat_get_touch(wl_seat);
            wl_touch_add_listener(seat->wl_touch, &touch_listener, seat);

            seat->touch.state = TOUCH_STATE_IDLE;
        }
    } else {
        if (seat->wl_touch != NULL) {
            wl_touch_release(seat->wl_touch);
            seat->wl_touch = NULL;
        }

        seat->touch.state = TOUCH_STATE_INHIBITED;
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
    const float old_scale = term->scale;
    const float logical_width = term->width / old_scale;
    const float logical_height = term->height / old_scale;

    /* Note: order matters! term_update_scale() must come first */
    bool scale_updated = term_update_scale(term);
    bool fonts_updated = term_font_dpi_changed(term, old_scale);
    term_font_subpixel_changed(term);

    csd_reload_font(term->window, old_scale);

    enum resize_options resize_opts = RESIZE_KEEP_GRID;

    if (fonts_updated) {
        /*
         * If the fonts have been updated, the cell dimensions have
         * changed. This requires a "forced" resize, since the surface
         * buffer dimensions may not have been updated (in which case
         * render_resize() normally shortcuts and returns early).
         */
        resize_opts |= RESIZE_FORCE;
    } else if (!scale_updated) {
        /* No need to resize if neither scale nor fonts have changed */
        return;
    } else if (term->conf->dpi_aware) {
        /*
	 * If fonts are sized according to DPI, it is possible for the cell
	 * size to remain the same when display scale changes. This will not
	 * change the surface buffer dimensions, but will change the logical
	 * size of the window. To ensure that the compositor is made aware of
	 * the proper logical size, force a resize rather than allowing
	 * render_resize() to shortcut the notification if the buffer
	 * dimensions remain the same.
	 */
        resize_opts |= RESIZE_FORCE;
    }

    render_resize(
        term,
        (int)roundf(logical_width),
        (int)roundf(logical_height),
        resize_opts);
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
    if (mon->dim.mm.width <= 0 || mon->dim.mm.height <= 0)
        return;

    double x_inches = mon->dim.mm.width * 0.03937008;
    double y_inches = mon->dim.mm.height * 0.03937008;

    const int width = mon->dim.px_real.width;
    const int height = mon->dim.px_real.height;

    mon->ppi.real.x = mon->dim.px_real.width / x_inches;
    mon->ppi.real.y = mon->dim.px_real.height / y_inches;

    /* The *logical* size is affected by the transform */
    switch (mon->transform) {
    case WL_OUTPUT_TRANSFORM_90:
    case WL_OUTPUT_TRANSFORM_270:
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
    case WL_OUTPUT_TRANSFORM_FLIPPED_270: {
        int swap = x_inches;
        x_inches = y_inches;
        y_inches = swap;
        break;
    }

    case WL_OUTPUT_TRANSFORM_NORMAL:
    case WL_OUTPUT_TRANSFORM_180:
    case WL_OUTPUT_TRANSFORM_FLIPPED:
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        break;
    }

    const int scaled_width = mon->dim.px_scaled.width;
    const int scaled_height = mon->dim.px_scaled.height;

    mon->ppi.scaled.x = scaled_width / x_inches;
    mon->ppi.scaled.y = scaled_height / y_inches;

    const double px_diag_physical = sqrt(pow(width, 2) + pow(height, 2));
    mon->dpi.physical = width == 0 && height == 0
        ? 96.
        : px_diag_physical / mon->inch;

    const double px_diag_scaled = sqrt(pow(scaled_width, 2) + pow(scaled_height, 2));
    mon->dpi.scaled = scaled_width == 0 && scaled_height == 0
        ? 96.
        : px_diag_scaled / mon->inch * mon->scale;

    if (mon->dpi.physical > 1000) {
        if (mon->name != NULL) {
            LOG_WARN("%s: DPI=%f (physical) is unreasonable, using 96 instead",
                     mon->name, mon->dpi.physical);
        }
        mon->dpi.physical = 96;
    }

    if (mon->dpi.scaled > 1000) {
        if (mon->name != NULL) {
            LOG_WARN("%s: DPI=%f (logical) is unreasonable, using 96 instead",
                     mon->name, mon->dpi.scaled);
        }
        mon->dpi.scaled = 96;
    }
}

static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                int32_t physical_width, int32_t physical_height,
                int32_t subpixel, const char *make, const char *model,
                int32_t transform)
{
    struct monitor *mon = data;

    free(mon->make);
    free(mon->model);

    mon->dim.mm.width = physical_width;
    mon->dim.mm.height = physical_height;
    mon->inch = sqrt(pow(mon->dim.mm.width, 2) + pow(mon->dim.mm.height, 2)) * 0.03937008;
    mon->make = make != NULL ? xstrdup(make) : NULL;
    mon->model = model != NULL ? xstrdup(model) : NULL;
    mon->subpixel = subpixel;
    mon->transform = transform;

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

#if defined(WL_OUTPUT_NAME_SINCE_VERSION)
static void
output_name(void *data, struct wl_output *wl_output, const char *name)
{
    struct monitor *mon = data;
    free(mon->name);
    mon->name = name != NULL ? xstrdup(name) : NULL;
}
#endif

#if defined(WL_OUTPUT_DESCRIPTION_SINCE_VERSION)
static void
output_description(void *data, struct wl_output *wl_output,
                   const char *description)
{
    struct monitor *mon = data;
    free(mon->description);
    mon->description = description != NULL ? xstrdup(description) : NULL;
}
#endif

static const struct wl_output_listener output_listener = {
    .geometry = &output_geometry,
    .mode = &output_mode,
    .done = &output_done,
    .scale = &output_scale,
#if defined(WL_OUTPUT_NAME_SINCE_VERSION)
    .name = &output_name,
#endif
#if defined(WL_OUTPUT_DESCRIPTION_SINCE_VERSION)
    .description = &output_description,
#endif
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
    struct monitor *mon = data;
    update_terms_on_monitor(mon);
}

static void
xdg_output_handle_name(void *data, struct zxdg_output_v1 *xdg_output,
                       const char *name)
{
    struct monitor *mon = data;
    free(mon->name);
    mon->name = name != NULL ? xstrdup(name) : NULL;
}

static void
xdg_output_handle_description(void *data, struct zxdg_output_v1 *xdg_output,
                              const char *description)
{
    struct monitor *mon = data;
    free(mon->description);
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

#if defined(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
static void
surface_preferred_buffer_scale(void *data, struct wl_surface *surface,
                               int32_t scale)
{
    struct wl_window *win = data;

    if (win->preferred_buffer_scale == scale)
        return;

    LOG_DBG("wl_surface preferred scale: %d -> %d", win->preferred_buffer_scale, scale);

    win->preferred_buffer_scale = scale;
    update_term_for_output_change(win->term);
}

static void
surface_preferred_buffer_transform(void *data, struct wl_surface *surface,
                                   uint32_t transform)
{

}
#endif

static const struct wl_surface_listener surface_listener = {
    .enter = &surface_enter,
    .leave = &surface_leave,
#if defined(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
    .preferred_buffer_scale = &surface_preferred_buffer_scale,
    .preferred_buffer_transform = &surface_preferred_buffer_transform,
#endif
};

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                       int32_t width, int32_t height, struct wl_array *states)
{
    bool is_activated = false;
    bool is_fullscreen = false;
    bool is_maximized = false;
    bool is_resizing = false;
    bool is_tiled_top = false;
    bool is_tiled_bottom = false;
    bool is_tiled_left = false;
    bool is_tiled_right = false;
    bool is_suspended UNUSED = false;

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
#if defined(XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION)  /* wayland-protocols >= 1.32 */
        [XDG_TOPLEVEL_STATE_SUSPENDED] = "suspended",
#endif
    };
#endif

    enum xdg_toplevel_state *state;
    wl_array_for_each(state, states) {
        switch (*state) {
        case XDG_TOPLEVEL_STATE_MAXIMIZED:    is_maximized = true; break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:   is_fullscreen = true; break;
        case XDG_TOPLEVEL_STATE_RESIZING:     is_resizing = true; break;
        case XDG_TOPLEVEL_STATE_ACTIVATED:    is_activated = true; break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:   is_tiled_left = true; break;
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:  is_tiled_right = true; break;
        case XDG_TOPLEVEL_STATE_TILED_TOP:    is_tiled_top = true; break;
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM: is_tiled_bottom = true; break;

#if defined(XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION)
        case XDG_TOPLEVEL_STATE_SUSPENDED:    is_suspended = true; break;
#endif
        }

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
        if (*state >= 0 && *state < ALEN(strings)) {
            state_chars += snprintf(
                &state_str[state_chars], sizeof(state_str) - state_chars,
                "%s, ",
                strings[*state] != NULL ? strings[*state] : "<unknown>");
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

    win->configure.is_activated = is_activated;
    win->configure.is_fullscreen = is_fullscreen;
    win->configure.is_maximized = is_maximized;
    win->configure.is_resizing = is_resizing;
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

#if defined(XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION)
static void
xdg_toplevel_configure_bounds(void *data,
                              struct xdg_toplevel *xdg_toplevel,
                              int32_t width, int32_t height)
{
    /* TODO: ensure we don't pick a bigger size */
}
#endif

#if defined(XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
static void
xdg_toplevel_wm_capabilities(void *data,
                             struct xdg_toplevel *xdg_toplevel,
                             struct wl_array *caps)
{
    struct wl_window *win = data;

    win->wm_capabilities.maximize = false;
    win->wm_capabilities.minimize = false;

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    char cap_str[2048];
    int cap_chars = 0;

    static const char *const strings[] = {
        [XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU] = "window-menu",
        [XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE] = "maximize",
        [XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN] = "fullscreen",
        [XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE] = "minimize",
    };
#endif

    enum xdg_toplevel_wm_capabilities *cap;
    wl_array_for_each(cap, caps) {
        switch (*cap) {
        case XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE:
            win->wm_capabilities.maximize = true;
            break;

        case XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE:
            win->wm_capabilities.minimize = true;
            break;

        case XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU:
        case XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN:
            break;
        }

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
        if (*cap >= 0 && *cap < ALEN(strings)) {
            cap_chars += snprintf(
                &cap_str[cap_chars], sizeof(cap_str) - cap_chars,
                "%s, ",
                 strings[*cap] != NULL ? strings[*cap] : "<unknown>");
        }
#endif
    }

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    if (cap_chars > 2)
        cap_str[cap_chars - 2] = '\0';
    else
        cap_str[0] = '\0';

    LOG_DBG("xdg-toplevel: wm-capabilities=[%s]", cap_str);
#endif
}
#endif

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = &xdg_toplevel_configure,
    /*.close = */&xdg_toplevel_close,  /* epoll-shim defines a macro 'close'... */
#if defined(XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION)
    .configure_bounds = &xdg_toplevel_configure_bounds,
#endif
#if defined(XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
    .wm_capabilities = xdg_toplevel_wm_capabilities,
#endif
};

static void
xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                      uint32_t serial)
{
    LOG_DBG("xdg-surface: configure");

    struct wl_window *win = data;
    struct terminal *term = win->term;

    if (win->unmapped) {
        /*
         * https://codeberg.org/dnkl/foot/issues/1249
         * https://gitlab.freedesktop.org/wlroots/wlroots/-/issues/3487
         * https://gitlab.freedesktop.org/wlroots/wlroots/-/merge_requests/3719
         * https://gitlab.freedesktop.org/wayland/wayland-protocols/-/issues/108
         */
        return;
    }

    bool wasnt_configured = !win->is_configured;
    bool was_resizing = win->is_resizing;
    bool csd_was_enabled = win->csd_mode == CSD_YES && !win->is_fullscreen;
    int new_width = win->configure.width;
    int new_height = win->configure.height;

    win->is_configured = true;
    win->is_maximized = win->configure.is_maximized;
    win->is_fullscreen = win->configure.is_fullscreen;
    win->is_resizing = win->configure.is_resizing;
    win->is_tiled_top = win->configure.is_tiled_top;
    win->is_tiled_bottom = win->configure.is_tiled_bottom;
    win->is_tiled_left = win->configure.is_tiled_left;
    win->is_tiled_right = win->configure.is_tiled_right;
    win->is_tiled = (win->is_tiled_top ||
                     win->is_tiled_bottom ||
                     win->is_tiled_left ||
                     win->is_tiled_right);
    win->csd_mode = win->configure.csd_mode;

    bool enable_csd = win->csd_mode == CSD_YES && !win->is_fullscreen;
    if (!csd_was_enabled && enable_csd)
        csd_instantiate(win);
    else if (csd_was_enabled && !enable_csd)
        csd_destroy(win);

    if (enable_csd && new_width > 0 && new_height > 0) {
        if (wayl_win_csd_titlebar_visible(win))
            new_height -= win->term->conf->csd.title_height;

        if (wayl_win_csd_borders_visible(win)) {
            new_height -= 2 * win->term->conf->csd.border_width_visible;
            new_width -= 2 * win->term->conf->csd.border_width_visible;
        }
    }

    xdg_surface_ack_configure(xdg_surface, serial);

    enum resize_options opts = RESIZE_BY_CELLS;

#if 1
    /*
     * TODO: decide if we should do the last "forced" call when ending
     * an interactive resize.
     *
     * Without it, the last TIOCSWINSZ sent to the client will be a
     * scheduled one. I.e. there will be a small delay after the user
     * has *stopped* resizing, and the client application receives the
     * final size.
     *
     * Note: if we also disable content centering while resizing, then
     * the last, forced, resize *is* necessary.
     */
    if (was_resizing && !win->is_resizing)
        opts |= RESIZE_FORCE;
#endif

    bool resized = render_resize(term, new_width, new_height, opts);

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
        wl_surface_commit(win->surface.surf);
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

    xassert(win->term->conf->csd.preferred != CONF_CSD_PREFER_NONE);
    switch (mode) {
    case ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
        LOG_INFO("using CSD decorations");
        win->configure.csd_mode = CSD_YES;
        break;

    case ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
        LOG_INFO("using SSD decorations");
        win->configure.csd_mode = CSD_NO;
        break;

    default:
        LOG_ERR("unimplemented: unknown XDG toplevel decoration mode: %u", mode);
        break;
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

    if (streq(interface, wl_compositor_interface.name)) {
        const uint32_t required = 4;
        if (!verify_iface_version(interface, version, required))
            return;

#if defined (WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
        const uint32_t preferred = WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION;
#else
        const uint32_t preferred = required;
#endif
        wayl->compositor = wl_registry_bind(
            wayl->registry, name, &wl_compositor_interface, min(version, preferred));
    }

    else if (streq(interface, wl_subcompositor_interface.name)) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->sub_compositor = wl_registry_bind(
            wayl->registry, name, &wl_subcompositor_interface, required);
    }

    else if (streq(interface, wl_shm_interface.name)) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

#if defined(WL_SHM_RELEASE_SINCE_VERSION)
        const uint32_t preferred = WL_SHM_RELEASE_SINCE_VERSION;
#else
        const uint32_t preferred = required;
#endif

        wayl->shm = wl_registry_bind(
            wayl->registry, name, &wl_shm_interface, min(version, preferred));
        wl_shm_add_listener(wayl->shm, &shm_listener, wayl);
#if defined(WL_SHM_RELEASE_SINCE_VERSION)
        wayl->use_shm_release = version >= WL_SHM_RELEASE_SINCE_VERSION;
#else
        wayl->use_shm_release = false;
#endif
    }

    else if (streq(interface, xdg_wm_base_interface.name)) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        /*
         * We *require* version 1, but _can_ use version 5. Version 2
         * adds 'tiled' window states. We use that information to
         * restore the window size when window is un-tiled. Version 5
         * adds 'wm_capabilities'. We use that information to draw
         * window decorations.
         */
#if defined(XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
        const uint32_t preferred = XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION;
#elif defined(XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION)
        const uint32_t preferred = XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION;
#else
        const uint32_t preferred = required;
#endif

        wayl->shell = wl_registry_bind(
            wayl->registry, name, &xdg_wm_base_interface, min(version, preferred));
        xdg_wm_base_add_listener(wayl->shell, &xdg_wm_base_listener, wayl);
    }

    else if (streq(interface, zxdg_decoration_manager_v1_interface.name)) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->xdg_decoration_manager = wl_registry_bind(
            wayl->registry, name, &zxdg_decoration_manager_v1_interface, required);
    }

    else if (streq(interface, wl_seat_interface.name)) {
        const uint32_t required = 5;
        if (!verify_iface_version(interface, version, required))
            return;

#if defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
        const uint32_t preferred = WL_POINTER_AXIS_VALUE120_SINCE_VERSION;
#else
        const uint32_t preferred = required;
#endif

        int repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (repeat_fd == -1) {
            LOG_ERRNO("failed to create keyboard repeat timer FD");
            return;
        }

        struct wl_seat *wl_seat = wl_registry_bind(
            wayl->registry, name, &wl_seat_interface, min(version, preferred));

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

        seat->kbd.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (seat->kbd.xkb != NULL) {
            seat->kbd.xkb_compose_table = xkb_compose_table_new_from_locale(
                seat->kbd.xkb, setlocale(LC_CTYPE, NULL), XKB_COMPOSE_COMPILE_NO_FLAGS);

            if (seat->kbd.xkb_compose_table != NULL) {
                seat->kbd.xkb_compose_state = xkb_compose_state_new(
                    seat->kbd.xkb_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
            } else {
                LOG_WARN("failed to instantiate compose table; dead keys (compose) will not work");
            }
        }

        seat_add_data_device(seat);
        seat_add_primary_selection(seat);
        seat_add_text_input(seat);
        seat_add_key_bindings(seat);
        wl_seat_add_listener(wl_seat, &seat_listener, seat);
    }

    else if (streq(interface, zxdg_output_manager_v1_interface.name)) {
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

    else if (streq(interface, wl_output_interface.name)) {
        const uint32_t required = 2;
        if (!verify_iface_version(interface, version, required))
            return;

#if defined(WL_OUTPUT_NAME_SINCE_VERSION)
        const uint32_t preferred = WL_OUTPUT_NAME_SINCE_VERSION;
#elif defined(WL_OUTPUT_RELEASE_SINCE_VERSION)
        const uint32_t preferred = WL_OUTPUT_RELEASE_SINCE_VERSION;
#else
        const uint32_t preferred = required;
#endif

        struct wl_output *output = wl_registry_bind(
            wayl->registry, name, &wl_output_interface, min(version, preferred));

        tll_push_back(
            wayl->monitors,
            ((struct monitor){.wayl = wayl, .output = output, .wl_name = name,
             .scale = 1,
             .use_output_release = version >= WL_OUTPUT_RELEASE_SINCE_VERSION}));

        struct monitor *mon = &tll_back(wayl->monitors);
        wl_output_add_listener(output, &output_listener, mon);

        if (wayl->xdg_output_manager != NULL) {
            mon->xdg = zxdg_output_manager_v1_get_xdg_output(
                wayl->xdg_output_manager, mon->output);
            zxdg_output_v1_add_listener(mon->xdg, &xdg_output_listener, mon);
        }
    }

    else if (streq(interface, wl_data_device_manager_interface.name)) {
        const uint32_t required = 3;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->data_device_manager = wl_registry_bind(
            wayl->registry, name, &wl_data_device_manager_interface, required);

        tll_foreach(wayl->seats, it)
            seat_add_data_device(&it->item);
    }

    else if (streq(interface, zwp_primary_selection_device_manager_v1_interface.name)) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->primary_selection_device_manager = wl_registry_bind(
            wayl->registry, name,
            &zwp_primary_selection_device_manager_v1_interface, required);

        tll_foreach(wayl->seats, it)
            seat_add_primary_selection(&it->item);
    }

    else if (streq(interface, wp_presentation_interface.name)) {
        if (wayl->presentation_timings) {
            const uint32_t required = 1;
            if (!verify_iface_version(interface, version, required))
                return;

            wayl->presentation = wl_registry_bind(
                wayl->registry, name, &wp_presentation_interface, required);
            wp_presentation_add_listener(
                wayl->presentation, &presentation_listener, wayl);
        }
    }

    else if (streq(interface, xdg_activation_v1_interface.name)) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->xdg_activation = wl_registry_bind(
            wayl->registry, name, &xdg_activation_v1_interface, required);
    }

    else if (streq(interface, wp_viewporter_interface.name)) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->viewporter = wl_registry_bind(
            wayl->registry, name, &wp_viewporter_interface, required);
    }

    else if (streq(interface, wp_fractional_scale_manager_v1_interface.name)) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->fractional_scale_manager = wl_registry_bind(
            wayl->registry, name,
            &wp_fractional_scale_manager_v1_interface, required);
    }

    else if (streq(interface, wp_cursor_shape_manager_v1_interface.name)) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->cursor_shape_manager = wl_registry_bind(
            wayl->registry, name, &wp_cursor_shape_manager_v1_interface, required);
    }

    else if (streq(interface, wp_single_pixel_buffer_manager_v1_interface.name)) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->single_pixel_manager = wl_registry_bind(
            wayl->registry, name,
            &wp_single_pixel_buffer_manager_v1_interface, required);
    }

#if defined(HAVE_XDG_TOPLEVEL_ICON)
    else if (streq(interface, xdg_toplevel_icon_v1_interface.name)) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->toplevel_icon_manager = wl_registry_bind(
            wayl->registry, name, &xdg_toplevel_icon_v1_interface, required);
    }
#endif

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    else if (streq(interface, zwp_text_input_manager_v3_interface.name)) {
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
                    seat, seat->wl_keyboard, -1, seat->kbd_focus->window->surface.surf);
        }

        if (seat->mouse_focus != NULL) {
            LOG_WARN("compositor destroyed seat '%s' "
                     "without sending a pointer leave event",
                     seat->name);

            if (seat->wl_pointer != NULL)
                pointer_listener.leave(
                    seat, seat->wl_pointer, -1, seat->mouse_focus->window->surface.surf);
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

        while (wl_display_prepare_read(wayl->display) != 0) {
            if (wl_display_dispatch_pending(wayl->display) < 0) {
                LOG_ERRNO("failed to dispatch pending Wayland events");
                return false;
            }
        }
    }

    if (events & EPOLLHUP) {
        LOG_WARN("disconnected from Wayland");
        /*
         * Do *not* call wl_display_cancel_read() here.
         *
         * Doing so causes later calls to wayl_roundtrip() (called
         * from term_destroy() -> wayl_win_destroy()) to hang
         * indefinitely.
         *
         * https://codeberg.org/dnkl/foot/issues/651
         */
        return false;
    }

    return event_count != -1;
}

struct wayland *
wayl_init(struct fdm *fdm, struct key_binding_manager *key_binding_manager,
          bool presentation_timings)
{
    struct wayland *wayl = calloc(1, sizeof(*wayl));
    if (unlikely(wayl == NULL)) {
        LOG_ERRNO("calloc() failed");
        return NULL;
    }

    wayl->fdm = fdm;
    wayl->key_binding_manager = key_binding_manager;
    wayl->fd = -1;
    wayl->presentation_timings = presentation_timings;

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
    if (tll_length(wayl->seats) == 0) {
        LOG_ERR("no seats available (wl_seat interface too old?)");
        goto out;
    }
    if (tll_length(wayl->monitors) == 0) {
        LOG_ERR("no monitors available");
        goto out;
    }

    if (presentation_timings && wayl->presentation == NULL) {
        LOG_ERR("compositor does not implement the presentation time interface");
        goto out;
    }

    if (wayl->primary_selection_device_manager == NULL)
        LOG_WARN("compositor does not implement the primary selection interface");

    if (wayl->xdg_activation == NULL) {
        LOG_WARN(
            "compositor does not implement XDG activation, "
            "bell.urgent will fall back to coloring the window margins red");
    }

    if (wayl->fractional_scale_manager == NULL || wayl->viewporter == NULL)
        LOG_WARN("compositor does not implement fractional scaling");

    if (wayl->cursor_shape_manager == NULL) {
        LOG_WARN("compositor does not implement server-side cursors, "
                 "falling back to client-side cursors");
    }

#if defined(HAVE_XDG_TOPLEVEL_ICON)
    if (wayl->toplevel_icon_manager == NULL) {
        LOG_WARN("compositor does not implement the XDG toplevel icon protocol");
    }
#endif

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (wayl->text_input_manager == NULL) {
        LOG_WARN("text input interface not implemented by compositor; "
                 "IME will be disabled");
    }
#endif

    /* Trigger listeners registered when handling globals */
    wl_display_roundtrip(wayl->display);

    tll_foreach(wayl->monitors, it) {
        LOG_INFO(
            "%s: %dx%d+%dx%d@%dHz %s %.2f\" scale=%d, DPI=%.2f/%.2f (physical/scaled)",
            it->item.name, it->item.dim.px_real.width, it->item.dim.px_real.height,
            it->item.x, it->item.y, (int)roundf(it->item.refresh),
            it->item.model != NULL ? it->item.model : it->item.description,
            it->item.inch, it->item.scale,
            it->item.dpi.physical, it->item.dpi.scaled);
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

    tll_foreach(wayl->monitors, it) {
        monitor_destroy(&it->item);
        tll_remove(wayl->monitors, it);
    }

    tll_foreach(wayl->seats, it) {
        seat_destroy(&it->item);
        tll_remove(wayl->seats, it);
    }

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (wayl->text_input_manager != NULL)
        zwp_text_input_manager_v3_destroy(wayl->text_input_manager);
#endif

#if defined(HAVE_XDG_TOPLEVEL_ICON)
    if (wayl->toplevel_icon_manager != NULL)
        xdg_toplevel_icon_manager_v1_destroy(wayl->toplevel_icon_manager);
#endif
    if (wayl->single_pixel_manager != NULL)
        wp_single_pixel_buffer_manager_v1_destroy(wayl->single_pixel_manager);
    if (wayl->fractional_scale_manager != NULL)
        wp_fractional_scale_manager_v1_destroy(wayl->fractional_scale_manager);
    if (wayl->viewporter != NULL)
        wp_viewporter_destroy(wayl->viewporter);
    if (wayl->cursor_shape_manager != NULL)
        wp_cursor_shape_manager_v1_destroy(wayl->cursor_shape_manager);
    if (wayl->xdg_activation != NULL)
        xdg_activation_v1_destroy(wayl->xdg_activation);
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
    if (wayl->shm != NULL) {
#if defined(WL_SHM_RELEASE_SINCE_VERSION)
        if (wayl->use_shm_release)
            wl_shm_release(wayl->shm);
        else
#endif
            wl_shm_destroy(wayl->shm);
    }
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

static void
fractional_scale_preferred_scale(
    void *data, struct wp_fractional_scale_v1 *wp_fractional_scale_v1,
    uint32_t scale)
{
    struct wl_window *win = data;

    const float new_scale = (float)scale / 120.;

    if (win->scale == new_scale)
        return;

    LOG_DBG("fractional scale: %.2f -> %.2f", win->scale, new_scale);

    win->scale = new_scale;
    update_term_for_output_change(win->term);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = &fractional_scale_preferred_scale,
};

struct wl_window *
wayl_win_init(struct terminal *term, const char *token)
{
    struct wayland *wayl = term->wl;
    const struct config *conf = term->conf;

    struct wl_window *win = calloc(1, sizeof(*win));
    if (unlikely(win == NULL)) {
        LOG_ERRNO("calloc() failed");
        return NULL;
    }

    win->term = term;
    win->csd_mode = CSD_UNKNOWN;
    win->csd.move_timeout_fd = -1;
    win->resize_timeout_fd = -1;
    win->scale = -1.;

    win->wm_capabilities.maximize = true;
    win->wm_capabilities.minimize = true;

    win->surface.surf = wl_compositor_create_surface(wayl->compositor);
    if (win->surface.surf == NULL) {
        LOG_ERR("failed to create wayland surface");
        goto out;
    }

    wayl_win_alpha_changed(win);

    wl_surface_add_listener(win->surface.surf, &surface_listener, win);

    if (wayl->fractional_scale_manager != NULL && wayl->viewporter != NULL) {
        win->surface.viewport = wp_viewporter_get_viewport(wayl->viewporter, win->surface.surf);

        win->fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(
                wayl->fractional_scale_manager, win->surface.surf);
        wp_fractional_scale_v1_add_listener(
            win->fractional_scale, &fractional_scale_listener, win);
    }

    win->xdg_surface = xdg_wm_base_get_xdg_surface(wayl->shell, win->surface.surf);
    xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, win);

    win->xdg_toplevel = xdg_surface_get_toplevel(win->xdg_surface);
    xdg_toplevel_add_listener(win->xdg_toplevel, &xdg_toplevel_listener, win);

    xdg_toplevel_set_app_id(win->xdg_toplevel, conf->app_id);

#if defined(HAVE_XDG_TOPLEVEL_ICON)
    if (wayl->toplevel_icon_manager != NULL) {
        const char *app_id =
            term->app_id != NULL ? term->app_id : term->conf->app_id;

        struct xdg_toplevel_icon_v1 *icon =
            xdg_toplevel_icon_manager_v1_create_icon(wayl->toplevel_icon_manager);
        xdg_toplevel_icon_v1_set_name(icon, streq(
            app_id, "footclient") ? "foot" : app_id);
        xdg_toplevel_icon_manager_v1_set_icon(
            wayl->toplevel_icon_manager, win->xdg_toplevel, icon);
        xdg_toplevel_icon_v1_destroy(icon);
    }
#endif

    if (conf->csd.preferred == CONF_CSD_PREFER_NONE) {
        /* User specifically do *not* want decorations */
        win->csd_mode = CSD_NO;
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
        win->configure.csd_mode = CSD_YES;
        LOG_WARN("no decoration manager available - using CSDs unconditionally");
    }

    wl_surface_commit(win->surface.surf);

    /* Complete XDG startup notification */
    wayl_activate(wayl, win, token);

    if (!wayl_win_subsurface_new(win, &win->overlay, false)) {
        LOG_ERR("failed to create overlay surface");
        goto out;
    }

    switch (conf->tweak.render_timer) {
    case RENDER_TIMER_OSD:
    case RENDER_TIMER_BOTH:
        if (!wayl_win_subsurface_new(win, &win->render_timer, false)) {
            LOG_ERR("failed to create render timer surface");
            goto out;
        }
        break;

    case RENDER_TIMER_NONE:
    case RENDER_TIMER_LOG:
        break;
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

    struct terminal *term = win->term;

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

    if (win->render_timer.surface.surf != NULL) {
        wl_surface_attach(win->render_timer.surface.surf, NULL, 0, 0);
        wl_surface_commit(win->render_timer.surface.surf);
    }

    if (win->scrollback_indicator.surface.surf != NULL) {
        wl_surface_attach(win->scrollback_indicator.surface.surf, NULL, 0, 0);
        wl_surface_commit(win->scrollback_indicator.surface.surf);
    }

    /* Scrollback search */
    if (win->search.surface.surf != NULL) {
        wl_surface_attach(win->search.surface.surf, NULL, 0, 0);
        wl_surface_commit(win->search.surface.surf);
    }

    /* URLs */
    tll_foreach(win->urls, it) {
        wl_surface_attach(it->item.surf.surface.surf, NULL, 0, 0);
        wl_surface_commit(it->item.surf.surface.surf);
    }

    /* CSD */
    for (size_t i = 0; i < ALEN(win->csd.surface); i++) {
        if (win->csd.surface[i].surface.surf != NULL) {
            wl_surface_attach(win->csd.surface[i].surface.surf, NULL, 0, 0);
            wl_surface_commit(win->csd.surface[i].surface.surf);
        }
    }

    wayl_roundtrip(win->term->wl);

        /* Main window */
    win->unmapped = true;
    wl_surface_attach(win->surface.surf, NULL, 0, 0);
    wl_surface_commit(win->surface.surf);
    wayl_roundtrip(win->term->wl);

    tll_free(win->on_outputs);

    tll_foreach(win->urls, it) {
        wayl_win_subsurface_destroy(&it->item.surf);
        tll_remove(win->urls, it);
    }

    csd_destroy(win);
    wayl_win_subsurface_destroy(&win->search);
    wayl_win_subsurface_destroy(&win->scrollback_indicator);
    wayl_win_subsurface_destroy(&win->render_timer);
    wayl_win_subsurface_destroy(&win->overlay);

    shm_purge(term->render.chains.search);
    shm_purge(term->render.chains.scrollback_indicator);
    shm_purge(term->render.chains.render_timer);
    shm_purge(term->render.chains.grid);
    shm_purge(term->render.chains.url);
    shm_purge(term->render.chains.csd);

    tll_foreach(win->xdg_tokens, it) {
        xdg_activation_token_v1_destroy(it->item->xdg_token);
        free(it->item);

        tll_remove(win->xdg_tokens, it);
    }

    if (win->fractional_scale != NULL)
        wp_fractional_scale_v1_destroy(win->fractional_scale);
    if (win->surface.viewport != NULL)
        wp_viewport_destroy(win->surface.viewport);
    if (win->frame_callback != NULL)
        wl_callback_destroy(win->frame_callback);
    if (win->xdg_toplevel_decoration != NULL)
        zxdg_toplevel_decoration_v1_destroy(win->xdg_toplevel_decoration);
    if (win->xdg_toplevel != NULL)
        xdg_toplevel_destroy(win->xdg_toplevel);
    if (win->xdg_surface != NULL)
        xdg_surface_destroy(win->xdg_surface);
    if (win->surface.surf != NULL)
        wl_surface_destroy(win->surface.surf);

    wayl_roundtrip(win->term->wl);

    if (win->resize_timeout_fd >= 0)
        fdm_del(win->term->wl->fdm, win->resize_timeout_fd);
    free(win);
}

bool
wayl_reload_xcursor_theme(struct seat *seat, float new_scale)
{
    if (seat->pointer.theme != NULL && seat->pointer.scale == new_scale) {
        /* We already have a theme loaded, and the scale hasn't changed */
        return true;
    }

    if (seat->pointer.theme != NULL) {
        xassert(seat->pointer.scale != new_scale);
        wl_cursor_theme_destroy(seat->pointer.theme);
        seat->pointer.theme = NULL;
        seat->pointer.cursor = NULL;
    }

    if (seat->pointer.shape_device != NULL) {
        /* Using server side cursors */
        return true;
    }

    int xcursor_size = 24;

    {
        const char *env_cursor_size = getenv("XCURSOR_SIZE");
        if (env_cursor_size != NULL) {
            errno = 0;
            char *end;
            int size = (int)strtol(env_cursor_size, &end, 10);

            if (errno == 0 && *end == '\0' && size > 0)
                xcursor_size = size;
            else
                LOG_WARN("XCURSOR_SIZE '%s' is invalid, defaulting to 24",
                         env_cursor_size);
        }
    }

    const char *xcursor_theme = getenv("XCURSOR_THEME");

    LOG_INFO("cursor theme: %s, size: %d, scale: %.2f",
             xcursor_theme ? xcursor_theme : "(null)",
             xcursor_size, new_scale);

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
        xassert(errno == EAGAIN);

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

            xassert(fds[0].revents & POLLOUT);
            break;
        }
    }
}

void
wayl_roundtrip(struct wayland *wayl)
{
    wl_display_cancel_read(wayl->display);
    if (wl_display_roundtrip(wayl->display) < 0) {
        LOG_ERRNO("failed to roundtrip Wayland display");
        return;
    }

    /* I suspect the roundtrip above clears the pending queue, and
     * that prepare_read() will always succeed in the first call. But,
     * better safe than sorry... */

    while (wl_display_prepare_read(wayl->display) != 0) {
        if (wl_display_dispatch_pending(wayl->display) < 0) {
            LOG_ERRNO("failed to dispatch pending Wayland events");
            return;
        }
    }
    wayl_flush(wayl);
}

static void
surface_scale_explicit_width_height(
    const struct wl_window *win, const struct wayl_surface *surf,
    int width, int height, float scale, bool verify)
{
    if (term_fractional_scaling(win->term)) {
        LOG_DBG("scaling by a factor of %.2f using fractional scaling "
                "(width=%d, height=%d) ", scale, width, height);

        if (verify) {
            if ((int)roundf(scale * (int)roundf(width / scale)) != width) {
                BUG("width=%d is not valid with scaling factor %.2f (%d != %d)",
                    width, scale,
                    (int)roundf(scale * (int)roundf(width / scale)),
                    width);
            }

            if ((int)roundf(scale * (int)roundf(height / scale)) != height) {
                BUG("height=%d is not valid with scaling factor %.2f (%d != %d)",
                    height, scale,
                    (int)roundf(scale * (int)roundf(height / scale)),
                    height);
            }
        }

        xassert(surf->viewport != NULL);
        wl_surface_set_buffer_scale(surf->surf, 1);
        wp_viewport_set_destination(
            surf->viewport, roundf(width / scale), roundf(height / scale));
    } else {
        const char *mode UNUSED = term_preferred_buffer_scale(win->term)
            ? "wl_surface.preferred_buffer_scale"
            : "legacy mode";
        LOG_DBG("scaling by a factor of %.2f using %s "
                "(width=%d, height=%d)" , scale, mode, width, height);

        xassert(scale == floorf(scale));
        const int iscale = (int)floorf(scale);

        if (verify) {
            if (width % iscale != 0) {
                BUG("width=%d is not valid with scaling factor %.2f (%d %% %d != 0)",
                    width, scale, width, iscale);
            }

            if (height % iscale != 0) {
                BUG("height=%d is not valid with scaling factor %.2f (%d %% %d != 0)",
                    height, scale, height, iscale);
            }
        }

        wl_surface_set_buffer_scale(surf->surf, iscale);
    }
}

void
wayl_surface_scale_explicit_width_height(
    const struct wl_window *win, const struct wayl_surface *surf,
    int width, int height, float scale)
{
    surface_scale_explicit_width_height(win, surf, width, height, scale, false);
}

void
wayl_surface_scale(const struct wl_window *win, const struct wayl_surface *surf,
                   const struct buffer *buf, float scale)
{
    surface_scale_explicit_width_height(
        win, surf, buf->width, buf->height, scale, true);
}

void
wayl_win_scale(struct wl_window *win, const struct buffer *buf)
{
    const struct terminal *term = win->term;
    const float scale = term->scale;

    wayl_surface_scale(win, &win->surface, buf, scale);
}

void
wayl_win_alpha_changed(struct wl_window *win)
{
    struct terminal *term = win->term;

    if (term->colors.alpha == 0xffff) {
        struct wl_region *region = wl_compositor_create_region(
            term->wl->compositor);

        if (region != NULL) {
            wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
            wl_surface_set_opaque_region(win->surface.surf, region);
            wl_region_destroy(region);
        }
    } else
        wl_surface_set_opaque_region(win->surface.surf, NULL);
}

static void
activation_token_for_urgency_done(const char *token, void *data)
{
    struct wl_window *win = data;
    struct wayland *wayl = win->term->wl;

    win->urgency_token_is_pending = false;
    xdg_activation_v1_activate(wayl->xdg_activation, token, win->surface.surf);
}

bool
wayl_win_set_urgent(struct wl_window *win)
{
    if (win->urgency_token_is_pending) {
        /* We already have a pending token. Don't request another one,
         * to avoid flooding the Wayland socket */
        return true;
    }

    bool success = wayl_get_activation_token(
        win->term->wl, NULL, 0, win, &activation_token_for_urgency_done, win);

    if (success) {
        win->urgency_token_is_pending = true;
        return true;
    }

    return false;
}

bool
wayl_win_csd_titlebar_visible(const struct wl_window *win)
{
    return win->csd_mode == CSD_YES &&
        !win->is_fullscreen &&
        !(win->is_maximized && win->term->conf->csd.hide_when_maximized);
}

bool
wayl_win_csd_borders_visible(const struct wl_window *win)
{
    return win->csd_mode == CSD_YES &&
        !win->is_fullscreen &&
        !win->is_maximized;
}

bool
wayl_win_subsurface_new_with_custom_parent(
    struct wl_window *win, struct wl_surface *parent,
    struct wayl_sub_surface *surf, bool allow_pointer_input)
{
    struct wayland *wayl = win->term->wl;

    surf->surface.surf = NULL;
    surf->sub = NULL;

    struct wl_surface *main_surface
        = wl_compositor_create_surface(wayl->compositor);

    if (main_surface == NULL) {
        LOG_ERR("failed to instantiate surface for sub-surface");
        return false;
    }

    struct wl_subsurface *sub = wl_subcompositor_get_subsurface(
        wayl->sub_compositor, main_surface, parent);

    if (sub == NULL) {
        LOG_ERR("failed to instantiate sub-surface");
        wl_surface_destroy(main_surface);
        return false;
    }

    struct wp_viewport *viewport = NULL;
    if (wayl->viewporter != NULL) {
        viewport = wp_viewporter_get_viewport(wayl->viewporter, main_surface);
        if (viewport == NULL) {
            LOG_ERR("failed to instantiate viewport for sub-surface");
            wl_subsurface_destroy(sub);
            wl_surface_destroy(main_surface);
            return false;
        }
    }

    wl_surface_set_user_data(main_surface, win);
    wl_subsurface_set_sync(sub);

    /* Disable pointer and touch events */
    if (!allow_pointer_input) {
        struct wl_region *empty =
            wl_compositor_create_region(wayl->compositor);
        wl_surface_set_input_region(main_surface, empty);
        wl_region_destroy(empty);
    }

    surf->surface.surf = main_surface;
    surf->sub = sub;
    surf->surface.viewport = viewport;
    return true;
}

bool
wayl_win_subsurface_new(struct wl_window *win, struct wayl_sub_surface *surf,
                        bool allow_pointer_input)
{
    return wayl_win_subsurface_new_with_custom_parent(
        win, win->surface.surf, surf, allow_pointer_input);
}

void
wayl_win_subsurface_destroy(struct wayl_sub_surface *surf)
{
    if (surf == NULL)
        return;

    if (surf->surface.viewport != NULL) {
        wp_viewport_destroy(surf->surface.viewport);
        surf->surface.viewport = NULL;
    }

    if (surf->sub != NULL) {
        wl_subsurface_destroy(surf->sub);
        surf->sub = NULL;
    }
    if (surf->surface.surf != NULL) {
        wl_surface_destroy(surf->surface.surf);
        surf->surface.surf = NULL;
    }
}

static void
activation_token_done(void *data, struct xdg_activation_token_v1 *xdg_token,
                      const char *token)
{
    LOG_DBG("XDG activation token done: %s", token);

    struct xdg_activation_token_context *ctx = data;
    struct wl_window *win = ctx->win;

    ctx->cb(token, ctx->cb_data);

    tll_foreach(win->xdg_tokens, it) {
        if (it->item->xdg_token != xdg_token)
            continue;

        xassert(win == it->item->win);

        free(ctx);
        xdg_activation_token_v1_destroy(xdg_token);
        tll_remove(win->xdg_tokens, it);
        return;
    }

    BUG("activation token not found in list");
}

static const struct
xdg_activation_token_v1_listener activation_token_listener = {
    .done = &activation_token_done,
};

bool
wayl_get_activation_token(
    struct wayland *wayl, struct seat *seat, uint32_t serial,
    struct wl_window *win,
    void (*cb)(const char *token, void *data), void *cb_data)
{
    if (wayl->xdg_activation == NULL)
        return false;

    struct xdg_activation_token_v1 *token =
        xdg_activation_v1_get_activation_token(wayl->xdg_activation);

    if (token == NULL) {
        LOG_ERR("failed to retrieve XDG activation token");
        return false;
    }

    struct xdg_activation_token_context *ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct xdg_activation_token_context){
        .win = win,
        .xdg_token = token,
        .cb = cb,
        .cb_data = cb_data,
    };
    tll_push_back(win->xdg_tokens, ctx);

    if (seat != NULL && serial != 0)
        xdg_activation_token_v1_set_serial(token, serial, seat->wl_seat);

    xdg_activation_token_v1_set_surface(token, win->surface.surf);
    xdg_activation_token_v1_add_listener(token, &activation_token_listener, ctx);
    xdg_activation_token_v1_commit(token);
    return true;
}

void
wayl_activate(struct wayland *wayl, struct wl_window *win, const char *token)
{
    if (wayl->xdg_activation == NULL)
        return;

    if (token == NULL)
        return;

    xdg_activation_v1_activate(wayl->xdg_activation, token, win->surface.surf);
}
