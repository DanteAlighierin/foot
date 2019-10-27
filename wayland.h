#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <wayland-client.h>
#include <primary-selection-unstable-v1.h>

#include "tllist.h"

struct monitor {
    struct terminal *term;
    struct wl_output *output;
    struct zxdg_output_v1 *xdg;
    char *name;

    int x;
    int y;

    int width_mm;
    int height_mm;

    int width_px;
    int height_px;

    int scale;
    float refresh;
};

struct wl_window {
    struct wl_surface *surface;
    struct xdg_wm_base *shell;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    struct zxdg_decoration_manager_v1 *xdg_decoration_manager;
    struct zxdg_toplevel_decoration_v1 *xdg_toplevel_decoration;

    /* Scrollback search */
    struct wl_surface *search_surface;
    struct wl_subsurface *search_sub_surface;

    struct wl_callback *frame_callback;

    tll(const struct monitor *) on_outputs; /* Outputs we're mapped on */
};

struct wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *sub_compositor;
    struct wl_shm *shm;

    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct zxdg_output_manager_v1 *xdg_output_manager;

    /* Clipboard */
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device;
    struct zwp_primary_selection_device_manager_v1 *primary_selection_device_manager;
    struct zwp_primary_selection_device_v1 *primary_selection_device;

    /* Cursor */
    struct {
        struct wl_pointer *pointer;
        uint32_t serial;

        struct wl_surface *surface;
        struct wl_cursor_theme *theme;
        struct wl_cursor *cursor;
        int size;
        char *theme_name;
    } pointer;

    bool have_argb8888;
    tll(struct monitor) monitors;  /* All available outputs */
};

/* TODO: return allocated pointer */
void wayl_init(struct wayland *wayl);
void wayl_destroy(struct wayland *wayl);

void wayl_win_destroy(struct wl_window *win);
