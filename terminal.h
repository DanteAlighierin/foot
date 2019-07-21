#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <threads.h>

#include <cairo.h>
#include <wayland-client.h>
#include <primary-selection-unstable-v1.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>


#include "tllist.h"

#define likely(c) __builtin_expect(!!(c), 1)
#define unlikely(c) __builtin_expect(!!(c), 0)

struct wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device;
    struct zwp_primary_selection_device_manager_v1 *primary_selection_device_manager;
    struct zwp_primary_selection_device_v1 *primary_selection_device;
    struct wl_keyboard *keyboard;
    struct {
        struct wl_pointer *pointer;
        uint32_t serial;

        struct wl_surface *surface;
        struct wl_cursor_theme *theme;
        struct wl_cursor *cursor;
    } pointer;
    struct xdg_wm_base *shell;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    bool have_argb8888;
};

struct rgb { float r, g, b; };

/*
 *  Note: we want the cells to be as small as possible. Larger cells
 *  means fewer scrollback lines (or performance drops due to cache
 *  misses) */
struct attributes {
    uint8_t bold:1;
    uint8_t dim:1;
    uint8_t italic:1;
    uint8_t underline:1;
    uint8_t strikethrough:1;
    uint8_t blink:1;
    uint8_t conceal:1;
    uint8_t reverse:1;

    uint32_t foreground;
    uint32_t background;
} __attribute__((packed));

struct cell {
    struct attributes attrs;
    char c[4];
} __attribute__((packed));

struct scroll_region {
    int start;
    int end;
};

struct coord {
    int col;
    int row;
};

enum damage_type {DAMAGE_SCROLL, DAMAGE_SCROLL_REVERSE};
struct damage {
    enum damage_type type;
    /* DAMAGE_SCROLL, DAMAGE_SCROLL_REVERSE */
    struct {
        struct scroll_region region;
        int lines;
    } scroll;
};

struct row {
    struct cell *cells;
    bool dirty;
};

struct grid {
    int num_rows;
    int num_cols;
    int offset;
    int view;

    struct row **rows;
    struct row *cur_row;

    tll(struct damage) damage;
    tll(struct damage) scroll_damage;
};

struct vt_subparams {
    unsigned value[16];
    size_t idx;
};

struct vt_param {
    unsigned value;
    struct vt_subparams sub;
};

struct vt {
    int state;  /* enum state */
    struct {
        struct vt_param v[16];
        size_t idx;
    } params;
    char private[2];
    struct {
        uint8_t *data;
        size_t size;
        size_t idx;
    } osc;
    struct {
        uint8_t *data;
        size_t size;
        size_t idx;
    } dcs;
    struct {
        uint8_t data[4];
        size_t idx;
        size_t left;
    } utf8;
    struct attributes attrs;
    struct attributes saved_attrs;
};

struct kbd {
    struct xkb_context *xkb;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    struct xkb_compose_table *xkb_compose_table;
    struct xkb_compose_state *xkb_compose_state;
    struct {
        mtx_t mutex;
        cnd_t cond;
        int trigger;
        int pipe_read_fd;
        int pipe_write_fd;
        enum {REPEAT_STOP, REPEAT_START, REPEAT_EXIT} cmd;

        bool dont_re_repeat;
        int32_t delay;
        int32_t rate;
        uint32_t key;
    } repeat;

    xkb_mod_index_t mod_shift;
    xkb_mod_index_t mod_alt;
    xkb_mod_index_t mod_ctrl;

    /* Enabled modifiers */
    bool shift;
    bool alt;
    bool ctrl;
};

enum cursor_keys { CURSOR_KEYS_DONTCARE, CURSOR_KEYS_NORMAL, CURSOR_KEYS_APPLICATION};
enum keypad_keys { KEYPAD_DONTCARE, KEYPAD_NUMERICAL, KEYPAD_APPLICATION };
enum charset { CHARSET_ASCII, CHARSET_GRAPHIC };

/* *What* to report */
enum mouse_tracking {
    MOUSE_NONE,
    MOUSE_X10,           /* ?9h */
    MOUSE_CLICK,         /* ?1000h - report mouse clicks*/
    MOUSE_DRAG,          /* ?1002h - report clicks and drag motions */
    MOUSE_MOTION,        /* ?1003h - report clicks and motion*/
};

/* *How* to report */
enum mouse_reporting {
    MOUSE_NORMAL,
    MOUSE_UTF8,          /* ?1005h */
    MOUSE_SGR,           /* ?1006h */
    MOUSE_URXVT,         /* ?1015h */
};

struct clipboard {
    struct wl_data_source *data_source;
    struct wl_data_offer *data_offer;
    char *text;
    uint32_t serial;
};

struct primary {
    struct zwp_primary_selection_source_v1 *data_source;
    struct zwp_primary_selection_offer_v1 *data_offer;
    char *text;
    uint32_t serial;
};

struct glyph_cache {
    cairo_glyph_t *glyphs;
    int count;
};

struct font {
    cairo_scaled_font_t *font;
    struct {
        double position;
        double thickness;
    } underline;
    struct {
        double position;
        double thickness;
    } strikeout;

    struct glyph_cache glyph_cache[256];
};

struct terminal {
    pid_t slave;
    int ptmx;
    bool quit;

    enum cursor_keys cursor_keys_mode;
    enum keypad_keys keypad_keys_mode;
    bool reverse;
    bool hide_cursor;
    bool auto_margin;
    bool insert_mode;
    bool bracketed_paste;
    bool focus_events;
    enum mouse_tracking mouse_tracking;
    enum mouse_reporting mouse_reporting;

    int selected_charset;
    enum charset charset[4]; /* G0-G3 */

    struct vt vt;
    struct kbd kbd;

    int width;  /* pixels */
    int height; /* pixels */
    int cols;   /* number of columns */
    int rows;   /* number of rows */
    int cell_width;  /* pixels per cell, x-wise */
    int cell_height; /* pixels per cell, y-wise */

    bool print_needs_wrap;
    struct scroll_region scroll_region;

    struct {
        uint32_t fg;
        uint32_t bg;
        uint32_t regular[8];
        uint32_t bright[8];

        uint32_t default_fg;
        uint32_t default_bg;
        uint32_t default_regular[8];
        uint32_t default_bright[8];
    } colors;

    struct {
        int col;
        int row;
        int button;

        int last_button;
        struct timeval last_time;
    } mouse;

    struct coord cursor;
    struct coord saved_cursor;
    struct coord alt_saved_cursor;

    uint32_t input_serial;
    struct {
        struct coord start;
        struct coord end;
        struct clipboard clipboard;
        struct primary primary;
    } selection;

    struct grid normal;
    struct grid alt;
    struct grid *grid;

    struct font fonts[4];
    cairo_font_extents_t fextents;

    struct wayland wl;
    struct wl_callback *frame_callback;
};

void term_damage_rows(struct terminal *term, int start, int end);
void term_damage_rows_in_view(struct terminal *term, int start, int end);

void term_damage_all(struct terminal *term);
void term_damage_view(struct terminal *term);

void term_damage_scroll(
    struct terminal *term, enum damage_type damage_type,
    struct scroll_region region, int lines);

void term_erase(
    struct terminal *term, const struct coord *start, const struct coord *end);

void term_cursor_to(struct terminal *term, int row, int col);
void term_cursor_left(struct terminal *term, int count);
void term_cursor_right(struct terminal *term, int count);
void term_cursor_up(struct terminal *term, int count);
void term_cursor_down(struct terminal *term, int count);

void term_scroll(struct terminal *term, int rows);
void term_scroll_reverse(struct terminal *term, int rows);

void term_scroll_partial(
    struct terminal *term, struct scroll_region region, int rows);
void term_scroll_reverse_partial(
    struct terminal *term, struct scroll_region region, int rows);

void term_linefeed(struct terminal *term);
void term_reverse_index(struct terminal *term);

void term_focus_in(struct terminal *term);
void term_focus_out(struct terminal *term);
void term_mouse_down(struct terminal *term, int button, int row, int col,
                     bool shift, bool alt, bool ctrl);
void term_mouse_up(struct terminal *term, int button, int row, int col,
                   bool shift, bool alt, bool ctrl);
void term_mouse_motion(struct terminal *term, int button, int row, int col,
                       bool shift, bool alt, bool ctrl);
