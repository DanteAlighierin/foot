#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#include <threads.h>
#include <semaphore.h>

#include "font.h"
#include "tllist.h"
#include "wayland.h"
#include "kbd.h"

#define likely(c) __builtin_expect(!!(c), 1)
#define unlikely(c) __builtin_expect(!!(c), 0)

struct rgb { float r, g, b; };

/*
 *  Note: we want the cells to be as small as possible. Larger cells
 *  means fewer scrollback lines (or performance drops due to cache
 *  misses)
 *
 * Note that the members are laid out optimized for x86
 */
struct attributes {
    uint32_t bold:1;
    uint32_t dim:1;
    uint32_t italic:1;
    uint32_t underline:1;
    uint32_t strikethrough:1;
    uint32_t blink:1;
    uint32_t conceal:1;
    uint32_t reverse:1;
    uint32_t fg:24;

    uint32_t clean:1;
    uint32_t have_fg:1;
    uint32_t have_bg:1;
    uint32_t reserved:5;
    uint32_t bg:24;
};
static_assert(sizeof(struct attributes) == 8, "bad size");

struct cell {
    wchar_t wc;
    struct attributes attrs;
};
static_assert(sizeof(struct cell) == 12, "bad size");

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
        uint8_t data[4];
        size_t idx;
        size_t left;
    } utf8;
    struct attributes attrs;
    struct attributes saved_attrs;
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

enum cursor_style { CURSOR_BLOCK, CURSOR_UNDERLINE, CURSOR_BAR };

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
    bool alt_scrolling;
    enum mouse_tracking mouse_tracking;
    enum mouse_reporting mouse_reporting;

    int selected_charset;
    enum charset charset[4]; /* G0-G3 */
    char *window_title;
    tll(char *) window_title_stack;

    struct {
        bool active;
        int fd;
    } flash;

    struct {
        bool active;
        enum { BLINK_ON, BLINK_OFF } state;
        int fd;
    } blink;

    struct vt vt;
    struct kbd kbd;

    int scale;
    int width;  /* pixels */
    int height; /* pixels */
    int x_margin;
    int y_margin;
    int cols;   /* number of columns */
    int rows;   /* number of rows */
    int cell_width;  /* pixels per cell, x-wise */
    int cell_height; /* pixels per cell, y-wise */

    bool print_needs_wrap;
    struct scroll_region scroll_region;

    struct {
        uint32_t fg;
        uint32_t bg;
        uint32_t table[256];
        double alpha;

        uint32_t default_fg;
        uint32_t default_bg;
        uint32_t default_table[256];
    } colors;

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

    struct coord cursor;
    struct coord saved_cursor;
    struct coord alt_saved_cursor;
    enum cursor_style default_cursor_style;
    enum cursor_style cursor_style;
    bool cursor_blinking;
    struct {
        uint32_t text;
        uint32_t cursor;
    } default_cursor_color;
    struct {
        uint32_t text;
        uint32_t cursor;
    } cursor_color;

    uint32_t input_serial;
    struct {
        struct coord start;
        struct coord end;
    } selection;

    bool is_searching;
    struct {
        wchar_t *buf;
        size_t len;
        size_t sz;
        size_t cursor;
        enum { SEARCH_BACKWARD, SEARCH_FORWARD} direction;

        int original_view;
        bool view_followed_offset;
        struct coord match;
        size_t match_len;
    } search;

    struct grid normal;
    struct grid alt;
    struct grid *grid;

    struct font *fonts[4];
    struct {
        int height;
        int descent;
        int ascent;
        int max_x_advance;
    } fextents;

    struct wayland wl;
    struct wl_window window;

    struct {
        int scrollback_lines;

        struct {
            size_t count;
            sem_t start;
            sem_t done;
            cnd_t cond;
            mtx_t lock;
            tll(int) queue;
            thrd_t *threads;
            struct buffer *buf;
        } workers;

        /* Last rendered cursor position */
        struct {
            struct coord actual;     /* Absolute */
            struct coord in_view;    /* Offset by view */
            struct cell *cell; /* For easy access to content */
        } last_cursor;

        struct buffer *last_buf;     /* Buffer we rendered to last time */
        bool was_flashing;           /* Flash was active last time we rendered */
        bool was_searching;
    } render;

    /* Temporary: for FDM */
    struct {
        bool is_armed;
        int lower_fd;
        int upper_fd;
    } delayed_render_timer;
};

void term_reset(struct terminal *term, bool hard);

void term_damage_rows(struct terminal *term, int start, int end);
void term_damage_rows_in_view(struct terminal *term, int start, int end);

void term_damage_all(struct terminal *term);
void term_damage_view(struct terminal *term);

void term_reset_view(struct terminal *term);

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

void term_restore_cursor(struct terminal *term);

void term_focus_in(struct terminal *term);
void term_focus_out(struct terminal *term);
void term_mouse_down(struct terminal *term, int button, int row, int col,
                     bool shift, bool alt, bool ctrl);
void term_mouse_up(struct terminal *term, int button, int row, int col,
                   bool shift, bool alt, bool ctrl);
void term_mouse_motion(struct terminal *term, int button, int row, int col,
                       bool shift, bool alt, bool ctrl);

void term_set_window_title(struct terminal *term, const char *title);
void term_flash(struct terminal *term, unsigned duration_ms);
