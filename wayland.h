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

/* Forward declarations */
struct terminal;

typedef tll(xkb_keycode_t) xkb_keycode_list_t;

struct key_binding {
    xkb_mod_mask_t mods;
    xkb_keysym_t sym;
    xkb_keycode_list_t key_codes;
};
typedef tll(struct key_binding) key_binding_list_t;

enum bind_action_normal {
    BIND_ACTION_NONE,
    BIND_ACTION_SCROLLBACK_UP_PAGE,
    BIND_ACTION_SCROLLBACK_UP_HALF_PAGE,
    BIND_ACTION_SCROLLBACK_UP_LINE,
    BIND_ACTION_SCROLLBACK_DOWN_PAGE,
    BIND_ACTION_SCROLLBACK_DOWN_HALF_PAGE,
    BIND_ACTION_SCROLLBACK_DOWN_LINE,
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
    BIND_ACTION_PIPE_SCROLLBACK,
    BIND_ACTION_PIPE_VIEW,
    BIND_ACTION_PIPE_SELECTED,

    /* Mouse specific actions - i.e. they require a mouse coordinate */
    BIND_ACTION_SELECT_BEGIN,
    BIND_ACTION_SELECT_BEGIN_BLOCK,
    BIND_ACTION_SELECT_EXTEND,
    BIND_ACTION_SELECT_EXTEND_CHAR_WISE,
    BIND_ACTION_SELECT_WORD,
    BIND_ACTION_SELECT_WORD_WS,
    BIND_ACTION_SELECT_ROW,

    BIND_ACTION_KEY_COUNT = BIND_ACTION_PIPE_SELECTED + 1,
    BIND_ACTION_COUNT = BIND_ACTION_SELECT_ROW + 1,
};

struct key_binding_normal {
    struct key_binding bind;
    enum bind_action_normal action;
    char **pipe_argv;
};

struct mouse_binding {
    enum bind_action_normal action;
    xkb_mod_mask_t mods;
    uint32_t button;
    int count;
    char **pipe_argv;
};
typedef tll(struct mouse_binding) mouse_binding_list_t;

enum bind_action_search {
    BIND_ACTION_SEARCH_NONE,
    BIND_ACTION_SEARCH_CANCEL,
    BIND_ACTION_SEARCH_COMMIT,
    BIND_ACTION_SEARCH_FIND_PREV,
    BIND_ACTION_SEARCH_FIND_NEXT,
    BIND_ACTION_SEARCH_EDIT_LEFT,
    BIND_ACTION_SEARCH_EDIT_LEFT_WORD,
    BIND_ACTION_SEARCH_EDIT_RIGHT,
    BIND_ACTION_SEARCH_EDIT_RIGHT_WORD,
    BIND_ACTION_SEARCH_EDIT_HOME,
    BIND_ACTION_SEARCH_EDIT_END,
    BIND_ACTION_SEARCH_DELETE_PREV,
    BIND_ACTION_SEARCH_DELETE_PREV_WORD,
    BIND_ACTION_SEARCH_DELETE_NEXT,
    BIND_ACTION_SEARCH_DELETE_NEXT_WORD,
    BIND_ACTION_SEARCH_EXTEND_WORD,
    BIND_ACTION_SEARCH_EXTEND_WORD_WS,
    BIND_ACTION_SEARCH_CLIPBOARD_PASTE,
    BIND_ACTION_SEARCH_PRIMARY_PASTE,
    BIND_ACTION_SEARCH_COUNT,
};

struct key_binding_search {
    struct key_binding bind;
    enum bind_action_search action;
};

/* Mime-types we support when dealing with data offers (e.g. copy-paste, or DnD) */
enum data_offer_mime_type {
    DATA_OFFER_MIME_UNSET,
    DATA_OFFER_MIME_TEXT_PLAIN,
    DATA_OFFER_MIME_TEXT_UTF8,
    DATA_OFFER_MIME_URI_LIST,
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

struct seat {
    struct wayland *wayl;
    struct wl_seat *wl_seat;
    uint32_t wl_name;
    char *name;

    /* Focused terminals */
    struct terminal *kbd_focus;
    struct terminal *mouse_focus;

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
        xkb_mod_index_t mod_meta;

        xkb_keycode_t key_arrow_up;
        xkb_keycode_t key_arrow_down;

        /* Enabled modifiers */
        bool shift;
        bool alt;
        bool ctrl;
        bool meta;

        struct {
            tll(struct key_binding_normal) key;
            tll(struct key_binding_search) search;
        } bindings;
    } kbd;

    /* Pointer state */
    struct wl_pointer *wl_pointer;
    struct {
        uint32_t serial;

        struct wl_surface *surface;
        struct wl_cursor_theme *theme;
        struct wl_cursor *cursor;
        int scale;
        bool hidden;

        const char *xcursor;
        struct wl_callback *xcursor_callback;
        bool xcursor_pending;
    } pointer;

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
        struct timeval last_time;

        /* We used a discrete axis event in the current pointer frame */
        double axis_aggregated;
        bool have_discrete;

        mouse_binding_list_t bindings;
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
            struct {
                char *text;
                int32_t cursor_begin;
                int32_t cursor_end;
            } pending;
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

    float dpi;

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

    struct wl_surface *scrollback_indicator_surface;
    struct wl_subsurface *scrollback_indicator_sub_surface;

    struct wl_surface *render_timer_surface;
    struct wl_subsurface *render_timer_sub_surface;

    struct wl_callback *frame_callback;

    tll(const struct monitor *) on_outputs; /* Outputs we're mapped on */

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
    } configure;

    int resize_timeout_fd;
};

struct config;
struct terminal;
struct wayland {
    const struct config *conf;
    struct fdm *fdm;

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

    struct wp_presentation *presentation;
    uint32_t presentation_clock_id;

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    struct zwp_text_input_manager_v3 *text_input_manager;
#endif

    bool have_argb8888;
    tll(struct monitor) monitors;  /* All available outputs */
    tll(struct seat) seats;

    tll(struct terminal *) terms;
};

struct wayland *wayl_init(const struct config *conf, struct fdm *fdm);
void wayl_destroy(struct wayland *wayl);

void wayl_flush(struct wayland *wayl);
void wayl_roundtrip(struct wayland *wayl);

struct wl_window *wayl_win_init(struct terminal *term);
void wayl_win_destroy(struct wl_window *win);

bool wayl_reload_xcursor_theme(struct seat *seat, int new_scale);
