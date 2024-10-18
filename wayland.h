#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <uchar.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

/* Wayland protocols */
#include <fractional-scale-v1.h>
#include <presentation-time.h>
#include <primary-selection-unstable-v1.h>
#include <single-pixel-buffer-v1.h>
#include <text-input-unstable-v3.h>
#include <viewporter.h>
#include <xdg-activation-v1.h>
#include <xdg-decoration-unstable-v1.h>
#include <xdg-output-unstable-v1.h>
#include <xdg-shell.h>

#if defined(HAVE_XDG_TOPLEVEL_ICON)
 #include <xdg-toplevel-icon-v1.h>
#endif

#include <fcft/fcft.h>
#include <tllist.h>

#include "cursor-shape.h"
#include "fdm.h"

/* Forward declarations */
struct terminal;
struct buffer;

/* Mime-types we support when dealing with data offers (e.g. copy-paste, or DnD) */
enum data_offer_mime_type {
    DATA_OFFER_MIME_UNSET,
    DATA_OFFER_MIME_TEXT_PLAIN,
    DATA_OFFER_MIME_TEXT_UTF8,
    DATA_OFFER_MIME_URI_LIST,

    DATA_OFFER_MIME_TEXT_TEXT,
    DATA_OFFER_MIME_TEXT_STRING,
    DATA_OFFER_MIME_TEXT_UTF8_STRING,
};

enum touch_state {
    TOUCH_STATE_INHIBITED = -1,
    TOUCH_STATE_IDLE,
    TOUCH_STATE_HELD,
    TOUCH_STATE_DRAGGING,
    TOUCH_STATE_SCROLLING,
};

struct wayl_surface {
    struct wl_surface *surf;
    struct wp_viewport *viewport;
};

struct wayl_sub_surface {
    struct wayl_surface surface;
    struct wl_subsurface *sub;
};

struct wl_window;
struct wl_clipboard {
    struct wl_window *window;  /* For DnD */
    struct wl_data_source *data_source;
    struct wl_data_offer *data_offer;
    enum data_offer_mime_type mime_type;
    char *text;
    uint32_t serial;
};

struct wl_primary {
    struct zwp_primary_selection_source_v1 *data_source;
    struct zwp_primary_selection_offer_v1 *data_offer;
    enum data_offer_mime_type mime_type;
    char *text;
    uint32_t serial;
};

/* Maps a mouse button to its "owning" surface */
struct button_tracker {
    int button;
    int surf_kind;  /* TODO: this is really an "enum term_surface" */
    bool send_to_client;  /* Only valid when surface is the main grid surface */
};

struct rect {
    int x;
    int y;
    int width;
    int height;
};

struct seat {
    struct wayland *wayl;
    struct wl_seat *wl_seat;
    uint32_t wl_name;
    char *name;

    /* Focused terminals */
    struct terminal *kbd_focus;
    struct terminal *mouse_focus;
    struct terminal *ime_focus;

    /* Keyboard state */
    struct wl_keyboard *wl_keyboard;
    struct {
        uint32_t serial;

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
        xkb_mod_index_t mod_super;
        xkb_mod_index_t mod_caps;
        xkb_mod_index_t mod_num;

        xkb_mod_mask_t legacy_significant;  /* Significant modifiers for the legacy keyboard protocol */
        xkb_mod_mask_t kitty_significant;   /* Significant modifiers for the kitty keyboard protocol */

        xkb_keycode_t key_arrow_up;
        xkb_keycode_t key_arrow_down;

        /* Enabled modifiers */
        bool shift;
        bool alt;
        bool ctrl;
        bool super;
    } kbd;

    /* Pointer state */
    struct wl_pointer *wl_pointer;
    struct {
        uint32_t serial;

        /* Client-side cursor */
        struct wayl_surface surface;
        struct wl_cursor_theme *theme;
        struct wl_cursor *cursor;

        /* Server-side cursor */
        struct wp_cursor_shape_device_v1 *shape_device;

        float scale;
        bool hidden;
        enum cursor_shape shape;
        char *last_custom_xcursor;

        struct wl_callback *xcursor_callback;
        bool xcursor_pending;
    } pointer;

    /* Touch state */
    struct wl_touch *wl_touch;
    struct {
        enum touch_state state;

        uint32_t serial;
        uint32_t time;
        struct wl_surface *surface;
        int surface_kind;
        int32_t id;
    } touch;

    struct {
        int x;
        int y;
        int col;
        int row;

        /* Mouse buttons currently being pressed, and their "owning" surfaces */
        tll(struct button_tracker) buttons;

        /* Double- and triple click state */
        int count;
        int last_released_button;
        struct timespec last_time;

        /* We used a discrete axis event in the current pointer frame */
        double aggregated[2];
        double aggregated_120[2];
        bool have_discrete;
    } mouse;

    /* Clipboard */
    struct wl_data_device *data_device;
    struct zwp_primary_selection_device_v1 *primary_selection_device;

    struct wl_clipboard clipboard;
    struct wl_primary primary;

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    /* Input Method Editor */
    struct zwp_text_input_v3 *wl_text_input;
    struct {
        struct {
            struct rect pending;
            struct rect sent;
        } cursor_rect;

        struct {
            struct {
                char *text;
                int32_t cursor_begin;
                int32_t cursor_end;
            } pending;

            char32_t *text;
            struct cell *cells;
            int count;
            struct {
                bool hidden;
                int start;  /* Cell index, inclusive */
                int end;    /* Cell index, exclusive */
            } cursor;
        } preedit;

        struct  {
            struct {
                char *text;
            } pending;
        } commit;

        struct {
            struct {
                uint32_t before_length;
                uint32_t after_length;
            } pending;
        } surrounding;

        uint32_t serial;
    } ime;
#endif
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

struct monitor {
    struct wayland *wayl;
    struct wl_output *output;
    struct zxdg_output_v1 *xdg;
    uint32_t wl_name;

    int x;
    int y;

    struct {
        /* Physical size, in mm */
        struct {
            int width;
            int height;
        } mm;

        /* Physical size, in pixels */
        struct {
            int width;
            int height;
        } px_real;

        /* Scaled size, in pixels */
        struct {
            int width;
            int height;
        } px_scaled;
    } dim;

    struct {
        /* PPI, based on physical size */
        struct {
            int x;
            int y;
        } real;

        /* PPI, logical, based on scaled size */
        struct {
            int x;
            int y;
        } scaled;
    } ppi;

    struct {
        float scaled;
        float physical;
    } dpi;

    int scale;
    float refresh;
    enum wl_output_subpixel subpixel;
    enum wl_output_transform transform;

    /* From wl_output */
    char *make;
    char *model;

    /* From xdg_output */
    char *name;
    char *description;

    float inch;  /* e.g. 24" */

    bool use_output_release;
};

struct wl_url {
    const struct url *url;
    struct wayl_sub_surface surf;
};

enum csd_mode {CSD_UNKNOWN, CSD_NO, CSD_YES};

typedef void (*activation_token_cb_t)(const char *token, void *data);

/*
 * This context holds data used both in the token::done callback, and
 * when cleaning up created, by not-yet-done tokens in
 * wayl_win_destroy().
 */
struct xdg_activation_token_context {
    struct wl_window *win;                        /* Need for win->xdg_tokens */
    struct xdg_activation_token_v1 *xdg_token;    /* Used to match token in done() */
    activation_token_cb_t cb;                     /* User provided callback */
    void *cb_data;                                /* Callback user pointer */
};

struct wayland;
struct wl_window {
    struct terminal *term;
    struct wayl_surface surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wp_fractional_scale_v1 *fractional_scale;

    tll(struct xdg_activation_token_context *) xdg_tokens;
    bool urgency_token_is_pending;

    bool unmapped;
    float scale;
    int preferred_buffer_scale;

    struct zxdg_toplevel_decoration_v1 *xdg_toplevel_decoration;

    enum csd_mode csd_mode;

    struct {
        struct wayl_sub_surface surface[CSD_SURF_COUNT];
        struct fcft_font *font;
        int move_timeout_fd;
        uint32_t serial;
    } csd;

    struct {
        bool maximize:1;
        bool minimize:1;
    } wm_capabilities;

    struct wayl_sub_surface search;
    struct wayl_sub_surface scrollback_indicator;
    struct wayl_sub_surface render_timer;
    struct wayl_sub_surface overlay;

    struct wl_callback *frame_callback;

    tll(const struct monitor *) on_outputs; /* Outputs we're mapped on */
    tll(struct wl_url) urls;

    bool is_configured;
    bool is_fullscreen;
    bool is_maximized;
    bool is_resizing;
    bool is_tiled_top;
    bool is_tiled_bottom;
    bool is_tiled_left;
    bool is_tiled_right;
    bool is_tiled;  /* At least one of is_tiled_{top,bottom,left,right} is true */
    struct {
        int width;
        int height;
        bool is_activated:1;
        bool is_fullscreen:1;
        bool is_maximized:1;
        bool is_resizing:1;
        bool is_tiled_top:1;
        bool is_tiled_bottom:1;
        bool is_tiled_left:1;
        bool is_tiled_right:1;
        enum csd_mode csd_mode;
    } configure;

    int resize_timeout_fd;
};

struct terminal;
struct wayland {
    struct fdm *fdm;
    struct key_binding_manager *key_binding_manager;

    int fd;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *sub_compositor;
    struct wl_shm *shm;

    struct zxdg_output_manager_v1 *xdg_output_manager;

    struct xdg_wm_base *shell;
    struct zxdg_decoration_manager_v1 *xdg_decoration_manager;

    struct wl_data_device_manager *data_device_manager;
    struct zwp_primary_selection_device_manager_v1 *primary_selection_device_manager;

    struct xdg_activation_v1 *xdg_activation;

    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager;

    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;

    struct wp_single_pixel_buffer_manager_v1 *single_pixel_manager;

#if defined(HAVE_XDG_TOPLEVEL_ICON)
    struct xdg_toplevel_icon_manager_v1 *toplevel_icon_manager;
#endif

    bool presentation_timings;
    struct wp_presentation *presentation;
    uint32_t presentation_clock_id;

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    struct zwp_text_input_manager_v3 *text_input_manager;
#endif

    tll(struct monitor) monitors;  /* All available outputs */
    tll(struct seat) seats;

    tll(struct terminal *) terms;

    /* WL_SHM >= 2 */
    bool use_shm_release;
};

struct wayland *wayl_init(
    struct fdm *fdm, struct key_binding_manager *key_binding_manager,
    bool presentation_timings);
void wayl_destroy(struct wayland *wayl);

bool wayl_reload_xcursor_theme(struct seat *seat, float new_scale);

void wayl_flush(struct wayland *wayl);
void wayl_roundtrip(struct wayland *wayl);

bool wayl_fractional_scaling(const struct wayland *wayl);
void wayl_surface_scale(
    const struct wl_window *win, const struct wayl_surface *surf,
    const struct buffer *buf, float scale);
void wayl_surface_scale_explicit_width_height(
    const struct wl_window *win, const struct wayl_surface *surf,
    int width, int height, float scale);

struct wl_window *wayl_win_init(struct terminal *term, const char *token);
void wayl_win_destroy(struct wl_window *win);

void wayl_win_scale(struct wl_window *win, const struct buffer *buf);
void wayl_win_alpha_changed(struct wl_window *win);
bool wayl_win_set_urgent(struct wl_window *win);

bool wayl_win_csd_titlebar_visible(const struct wl_window *win);
bool wayl_win_csd_borders_visible(const struct wl_window *win);

bool wayl_win_subsurface_new(
    struct wl_window *win, struct wayl_sub_surface *surf,
    bool allow_pointer_input);
bool wayl_win_subsurface_new_with_custom_parent(
    struct wl_window *win, struct wl_surface *parent,
    struct wayl_sub_surface *surf, bool allow_pointer_input);
void wayl_win_subsurface_destroy(struct wayl_sub_surface *surf);

bool wayl_get_activation_token(
    struct wayland *wayl, struct seat *seat, uint32_t serial,
    struct wl_window *win, activation_token_cb_t cb, void *cb_data);
void wayl_activate(struct wayland *wayl, struct wl_window *win, const char *token);

