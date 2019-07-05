#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <threads.h>

#include <cairo.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "tllist.h"

struct wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    struct wl_shm *shm;
    struct wl_seat *seat;
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
};

struct rgba { double r, g, b, a; };

struct attributes {
    bool bold;
    bool italic;
    bool underline;
    bool strikethrough;
    bool blink;
    bool conceal;
    bool reverse;
    bool have_foreground;
    bool have_background;
    struct rgba foreground; /* Only valid when have_foreground == true */
    struct rgba background; /* Only valid when have_background == true */
};

struct cell {
    char c[5];
    struct attributes attrs;
};

struct scroll_region {
    int start;
    int end;
};

struct cursor {
    int row;
    int col;
    int linear;
};

enum damage_type {DAMAGE_UPDATE, DAMAGE_ERASE, DAMAGE_SCROLL, DAMAGE_SCROLL_REVERSE};
struct damage {
    enum damage_type type;
    union {
        /* DAMAGE_UPDATE, DAMAGE_ERASE */
        struct {
            int start;
            int length;
        } range;

        /* DAMAGE_SCROLL, DAMAGE_SCROLL_REVERSE */
        struct {
            struct scroll_region region;
            int lines;
        } scroll;
    };
};

struct grid {
    int size;
    int offset;

    struct cell *cells;
    struct cell *cur_line;

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
    struct {
        uint8_t data[2];
        size_t idx;
    } intermediates;
    struct {
        uint8_t data[1024];
        size_t idx;
    } osc;
    struct {
        uint8_t data[4];
        size_t idx;
        size_t left;
    } utf8;
    struct attributes attrs;
    struct attributes saved_attrs;
    bool dim;
};

struct kbd {
    struct xkb_context *xkb;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
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
};

enum decckm { DECCKM_CSI, DECCKM_SS3 };
enum keypad_mode { KEYPAD_NUMERICAL, KEYPAD_APPLICATION };
enum charset { CHARSET_ASCII, CHARSET_GRAPHIC };

struct terminal {
    pid_t slave;
    int ptmx;
    bool quit;

    enum decckm decckm;
    enum keypad_mode keypad_mode;
    bool hide_cursor;
    bool auto_margin;
    bool insert_mode;
    bool bracketed_paste;

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

    struct rgba foreground;
    struct rgba background;

    struct cursor cursor;
    struct cursor saved_cursor;
    struct cursor alt_saved_cursor;

    struct grid normal;
    struct grid alt;
    struct grid *grid;

    cairo_scaled_font_t *fonts[4];
    cairo_font_extents_t fextents;

    struct wayland wl;
    bool frame_is_scheduled;
};

void term_damage_all(struct terminal *term);
void term_damage_update(struct terminal *term, int start, int length);
void term_damage_erase(struct terminal *term, int start, int length);
void term_damage_scroll(
    struct terminal *term, enum damage_type damage_type,
    struct scroll_region region, int lines);

void term_erase(struct terminal *term, int start, int end);

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

int term_cursor_linear(const struct terminal *term, int row, int col);
