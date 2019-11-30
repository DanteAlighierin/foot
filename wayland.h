#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <sys/time.h>

#include <wayland-client.h>
#include <primary-selection-unstable-v1.h>
#include <xkbcommon/xkbcommon.h>

#include "fdm.h"
#include "tllist.h"

struct monitor {
    struct wayland *wayl;
    struct wl_output *output;
    struct zxdg_output_v1 *xdg;
    char *name;

    int x;
    int y;

    int width_mm;
    int height_mm;

    int width_px;
    int height_px;

    int x_ppi;
    int y_ppi;

    int scale;
    float refresh;
};

struct kbd {
    struct xkb_context *xkb;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    struct xkb_compose_table *xkb_compose_table;
    struct xkb_compose_state *xkb_compose_state;
    struct {
        int fd;

        bool dont_re_repeat;
        int32_t delay;
        int32_t rate;
        uint32_t key;
    } repeat;

    xkb_mod_index_t mod_shift;
    xkb_mod_index_t mod_alt;
    xkb_mod_index_t mod_ctrl;
    xkb_mod_index_t mod_meta;

    /* Enabled modifiers */
    bool shift;
    bool alt;
    bool ctrl;
    bool meta;
};

struct wl_clipboard {
    struct wl_data_source *data_source;
    struct wl_data_offer *data_offer;
    char *text;
    uint32_t serial;
};

struct wl_primary {
    struct zwp_primary_selection_source_v1 *data_source;
    struct zwp_primary_selection_offer_v1 *data_offer;
    char *text;
    uint32_t serial;
};

struct wayland;
struct wl_window {
    struct wayland *wayl;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    struct zxdg_toplevel_decoration_v1 *xdg_toplevel_decoration;

    /* Scrollback search */
    struct wl_surface *search_surface;
    struct wl_subsurface *search_sub_surface;

    struct wl_callback *frame_callback;

    tll(const struct monitor *) on_outputs; /* Outputs we're mapped on */
};

struct terminal;
struct wayland {
    struct fdm *fdm;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *sub_compositor;
    struct wl_shm *shm;

    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct zxdg_output_manager_v1 *xdg_output_manager;

    struct xdg_wm_base *shell;
    struct zxdg_decoration_manager_v1 *xdg_decoration_manager;

    /* Keyboard */
    struct kbd kbd;

    /* Clipboard */
    uint32_t input_serial;
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device;
    struct zwp_primary_selection_device_manager_v1 *primary_selection_device_manager;
    struct zwp_primary_selection_device_v1 *primary_selection_device;

    struct wl_clipboard clipboard;
    struct wl_primary primary;

    /* Cursor */
    struct {
        struct wl_pointer *pointer;
        uint32_t serial;

        struct wl_surface *surface;
        struct wl_cursor_theme *theme;
        struct wl_cursor *cursor;
        int size;
        char *theme_name;
        const char *xcursor;
    } pointer;

    struct {
        int col;
        int row;
        int button;

        int count;
        int last_button;
        struct timeval last_time;

        /* We used a discrete axis event in the current pointer frame */
        bool have_discrete;
    } mouse;

    bool have_argb8888;
    tll(struct monitor) monitors;  /* All available outputs */

    tll(struct terminal *) terms;
    struct terminal *focused;
    struct terminal *moused;
};

struct wayland *wayl_init(struct fdm *fdm);
void wayl_destroy(struct wayland *wayl);

struct terminal *wayl_terminal_from_surface(
    struct wayland *wayl, struct wl_surface *surface);
struct terminal *wayl_terminal_from_xdg_surface(
    struct wayland *wayl, struct xdg_surface *surface);
struct terminal *wayl_terminal_from_xdg_toplevel(
    struct wayland *wayl, struct xdg_toplevel *toplevel);

/* TODO: pass something other than 'term'? Need scale... */
bool wayl_cursor_set(struct wayland *wayl, const struct terminal *term);

struct wl_window *wayl_win_init(struct wayland *wayl);
void wayl_win_destroy(struct wl_window *win);
