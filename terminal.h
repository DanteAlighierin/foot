#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#include <threads.h>
#include <semaphore.h>

#include <tllist.h>
#include <fcft/fcft.h>

//#include "config.h"
#include "fdm.h"
#include "wayland.h"

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
    uint32_t selected:2;
    uint32_t reserved:3;
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

struct cursor {
    struct coord point;
    bool lcf;
};

enum damage_type {DAMAGE_SCROLL, DAMAGE_SCROLL_REVERSE,
                  DAMAGE_SCROLL_IN_VIEW, DAMAGE_SCROLL_REVERSE_IN_VIEW};

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
    bool linebreak;
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
    uint8_t idx;
};

struct vt_param {
    unsigned value;
    struct vt_subparams sub;
};

struct vt {
    int state;  /* enum state */
    wchar_t last_printed;
    struct {
        struct vt_param v[16];
        uint8_t idx;
    } params;
    char private[2];
    struct {
        uint8_t *data;
        size_t size;
        size_t idx;
    } osc;
    struct {
        uint8_t data[4];
        uint8_t idx;
        uint8_t left;
    } utf8;
    struct {
        uint8_t *data;
        size_t size;
        size_t idx;
        void (*put_handler)(struct terminal *term, uint8_t c);
        void (*unhook_handler)(struct terminal *term);
    } dcs;
    struct attributes attrs;
    struct attributes saved_attrs;
};

enum cursor_origin { ORIGIN_ABSOLUTE, ORIGIN_RELATIVE };
enum cursor_keys { CURSOR_KEYS_DONTCARE, CURSOR_KEYS_NORMAL, CURSOR_KEYS_APPLICATION };
enum keypad_keys { KEYPAD_DONTCARE, KEYPAD_NUMERICAL, KEYPAD_APPLICATION };
enum charset { CHARSET_ASCII, CHARSET_GRAPHIC };

struct charsets {
    int selected;
    enum charset set[4]; /* G0-G3 */
};

/* *What* to report */
enum mouse_tracking {
    MOUSE_NONE,
    MOUSE_X10,           /* ?9h */
    MOUSE_CLICK,         /* ?1000h - report mouse clicks */
    MOUSE_DRAG,          /* ?1002h - report clicks and drag motions */
    MOUSE_MOTION,        /* ?1003h - report clicks and motion */
};

/* *How* to report */
enum mouse_reporting {
    MOUSE_NORMAL,
    MOUSE_UTF8,          /* ?1005h */
    MOUSE_SGR,           /* ?1006h */
    MOUSE_URXVT,         /* ?1015h */
};

enum cursor_style { CURSOR_BLOCK, CURSOR_UNDERLINE, CURSOR_BAR };

enum selection_kind { SELECTION_NONE, SELECTION_NORMAL, SELECTION_BLOCK };

struct ptmx_buffer {
    void *data;
    size_t len;
    size_t idx;
};

struct sixel {
    void *data;
    pixman_image_t *pix;
    int width;
    int height;
    int rows;
    struct coord pos;
};

struct terminal {
    struct fdm *fdm;
    const struct config *conf;

    pid_t slave;
    int ptmx;
    bool quit;

    struct grid normal;
    struct grid alt;
    struct grid *grid;

    struct font *fonts[4];
    int font_dpi;
    int font_adjustments;

    tll(struct ptmx_buffer) ptmx_buffer;

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

    struct charsets charsets;
    struct charsets saved_charsets; /* For save/restore cursor + attributes */

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

    int scale;
    int width;  /* pixels */
    int height; /* pixels */
    int x_margin;
    int y_margin;
    int cols;   /* number of columns */
    int rows;   /* number of rows */
    int cell_width;  /* pixels per cell, x-wise */
    int cell_height; /* pixels per cell, y-wise */

    struct scroll_region scroll_region;

    struct {
        uint32_t fg;
        uint32_t bg;
        uint32_t table[256];
        uint16_t alpha;

        uint32_t default_fg;
        uint32_t default_bg;
        uint32_t default_table[256];
    } colors;

    enum cursor_origin origin;
    struct cursor cursor;
    struct cursor saved_cursor;
    struct cursor alt_saved_cursor;
    enum cursor_style default_cursor_style;
    enum cursor_style cursor_style;
    struct {
        bool active;
        enum { CURSOR_BLINK_ON, CURSOR_BLINK_OFF } state;
        int fd;
    } cursor_blink;
    struct {
        uint32_t text;
        uint32_t cursor;
    } default_cursor_color;
    struct {
        uint32_t text;
        uint32_t cursor;
    } cursor_color;
    const char *xcursor;

    struct {
        enum selection_kind kind;
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

    struct {
        bool esc_prefix;
        bool eight_bit;
    } meta;

    tll(int) tab_stops;

    struct wayland *wl;
    struct wl_window *window;
    bool visual_focus;

    struct {
        bool refresh_needed;  /* Terminal needs to be re-rendered, as soon-as-possible */
        int scrollback_lines; /* Number of scrollback lines, from conf (TODO: move out from render struct?) */

        struct {
            bool enabled;
            int timer_fd;
        } app_sync_updates;

        /* Render threads + synchronization primitives */
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

        bool pending;                /* Need to re-render again, after next frame-callback */
        struct buffer *last_buf;     /* Buffer we rendered to last time */
        bool was_flashing;           /* Flash was active last time we rendered */
        bool was_searching;

        size_t search_glyph_offset;

        bool presentation_timings;
        struct timespec input_time;
    } render;

    /* Temporary: for FDM */
    struct {
        bool is_armed;
        int lower_fd;
        int upper_fd;
    } delayed_render_timer;

    struct {
        enum {
            SIXEL_DECSIXEL,  /* DECSIXEL body part ", $, -, ? ... ~ */
            SIXEL_DECGRA,    /* DECGRA Set Raster Attributes " Pan; Pad; Ph; Pv */
            SIXEL_DECGRI,    /* DECGRI Graphics Repeat Introducer ! Pn Ch */
            SIXEL_DECGCI,    /* DECGCI Graphics Color Introducer # Pc; Pu; Px; Py; Pz */        
        } state;

        struct coord pos;    /* Current sixel coordinate */
        int color_idx;       /* Current palette index */
        int max_col;         /* Largest column index we've seen (aka the image width) */
        uint32_t *palette;   /* Color palette */

        struct {
            uint32_t *data;  /* Raw image data, in ARGB */
            int width;       /* Image width, in pixels */
            int height;      /* Image height, in pixels */
        } image;

        unsigned params[5];  /* Collected parmaeters, for RASTER, COLOR_SPEC */
        unsigned param;      /* Currently collecting parameter, for RASTER, COLOR_SPEC and REPEAT */
        unsigned param_idx;  /* Parameters seen */
    } sixel;

    tll(struct sixel) sixel_images;

    bool hold_at_exit;
    bool is_shutting_down;
    void (*shutdown_cb)(void *data, int exit_code);
    void *shutdown_data;

    char *foot_exe;
    char *cwd;
};

struct config;
struct terminal *term_init(
    const struct config *conf, struct fdm *fdm, struct wayland *wayl,
    const char *term_env, bool login_shell, const char *foot_exe,
    const char *cwd, int argc, char *const *argv,
    void (*shutdown_cb)(void *data, int exit_code), void *shutdown_data);

bool term_shutdown(struct terminal *term);
int term_destroy(struct terminal *term);

void term_reset(struct terminal *term, bool hard);
bool term_to_slave(struct terminal *term, const void *data, size_t len);

bool term_font_size_increase(struct terminal *term);
bool term_font_size_decrease(struct terminal *term);
bool term_font_size_reset(struct terminal *term);
bool term_font_dpi_changed(struct terminal *term);

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

int term_row_rel_to_abs(const struct terminal *term, int row);
void term_cursor_home(struct terminal *term);
void term_cursor_to(struct terminal *term, int row, int col);
void term_cursor_left(struct terminal *term, int count);
void term_cursor_right(struct terminal *term, int count);
void term_cursor_up(struct terminal *term, int count);
void term_cursor_down(struct terminal *term, int count);
void term_cursor_blink_enable(struct terminal *term);
void term_cursor_blink_disable(struct terminal *term);
void term_cursor_blink_restart(struct terminal *term);

void term_print(struct terminal *term, wchar_t wc, int width);

void term_scroll(struct terminal *term, int rows);
void term_scroll_reverse(struct terminal *term, int rows);

void term_scroll_partial(
    struct terminal *term, struct scroll_region region, int rows);
void term_scroll_reverse_partial(
    struct terminal *term, struct scroll_region region, int rows);

void term_formfeed(struct terminal *term);
void term_linefeed(struct terminal *term);
void term_reverse_index(struct terminal *term);

void term_arm_blink_timer(struct terminal *term);

void term_restore_cursor(struct terminal *term);

void term_visual_focus_in(struct terminal *term);
void term_visual_focus_out(struct terminal *term);
void term_kbd_focus_in(struct terminal *term);
void term_kbd_focus_out(struct terminal *term);
void term_mouse_down(struct terminal *term, int button, int row, int col);
void term_mouse_up(struct terminal *term, int button, int row, int col);
void term_mouse_motion(struct terminal *term, int button, int row, int col);
bool term_mouse_grabbed(const struct terminal *term);
void term_xcursor_update(struct terminal *term);

void term_set_window_title(struct terminal *term, const char *title);
void term_flash(struct terminal *term, unsigned duration_ms);
bool term_spawn_new(const struct terminal *term);

void term_enable_app_sync_updates(struct terminal *term);
void term_disable_app_sync_updates(struct terminal *term);
