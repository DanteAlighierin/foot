#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <sys/time.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <primary-selection-unstable-v1.h>
#include <presentation-time.h>

#include <tllist.h>

#include "fdm.h"

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

    char *make;
    char *model;
    float inch;  /* e.g. 24" */
};

enum binding_action {
    BIND_ACTION_SCROLLBACK_UP,
    BIND_ACTION_SCROLLBACK_DOWN,
    BIND_ACTION_CLIPBOARD_COPY,
    BIND_ACTION_CLIPBOARD_PASTE,
    BIND_ACTION_PRIMARY_PASTE,
    BIND_ACTION_SEARCH_START,
    BIND_ACTION_FONT_SIZE_UP,
    BIND_ACTION_FONT_SIZE_DOWN,
    BIND_ACTION_FONT_SIZE_RESET,
    BIND_ACTION_SPAWN_TERMINAL,
    BIND_ACTION_MINIMIZE,
    BIND_ACTION_MAXIMIZE,
    BIND_ACTION_FULLSCREEN,
    BIND_ACTION_COUNT,
};

struct key_binding {
    xkb_mod_mask_t mods;
    xkb_keysym_t sym;
    enum binding_action action;
};
typedef tll(struct key_binding) key_binding_list_t;

struct mouse_binding {
    uint32_t button;
    int count;
    enum binding_action action;
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

    struct {
        key_binding_list_t key;
        key_binding_list_t search;
    } bindings;
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

enum csd_surface {
    CSD_SURF_TITLE,
    CSD_SURF_LEFT,
    CSD_SURF_RIGHT,
    CSD_SURF_TOP,
    CSD_SURF_BOTTOM,
    CSD_SURF_MINIMIZE,
    CSD_SURF_MAXIMIZE,
    CSD_SURF_CLOSE,
    CSD_SURF_COUNT,
};

struct wayland;
struct wl_window {
    struct terminal *term;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    struct zxdg_toplevel_decoration_v1 *xdg_toplevel_decoration;

    enum {CSD_UNKNOWN, CSD_NO, CSD_YES } use_csd;

    struct {
        struct wl_surface *surface[CSD_SURF_COUNT];
        struct wl_subsurface *sub_surface[CSD_SURF_COUNT];
        int move_timeout_fd;
        uint32_t serial;
    } csd;

    /* Scrollback search */
    struct wl_surface *search_surface;
    struct wl_subsurface *search_sub_surface;

    struct wl_callback *frame_callback;

    tll(const struct monitor *) on_outputs; /* Outputs we're mapped on */

    bool is_configured;
    bool is_fullscreen;
    bool is_maximized;
    struct {
        bool is_activated;
        bool is_fullscreen;
        bool is_maximized;
        int width;
        int height;
    } configure;
};

struct config;
struct terminal;
struct wayland {
    const struct config *conf;
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

    struct wp_presentation *presentation;
    uint32_t presentation_clock_id;

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

        const struct terminal *pending_terminal;
        struct wl_callback *xcursor_callback;
    } pointer;

    struct {
        int x;
        int y;
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
    struct terminal *kbd_focus;
    struct terminal *mouse_focus;
};

struct wayland *wayl_init(const struct config *conf, struct fdm *fdm);
void wayl_destroy(struct wayland *wayl);

void wayl_flush(struct wayland *wayl);
void wayl_roundtrip(struct wayland *wayl);

struct wl_window *wayl_win_init(struct terminal *term);
void wayl_win_destroy(struct wl_window *win);
