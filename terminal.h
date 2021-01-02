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
#include "macros.h"
#include "reaper.h"
#include "wayland.h"

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

#define CELL_COMB_CHARS_LO    0x40000000ul
#define CELL_COMB_CHARS_HI    0x400ffffful
#define CELL_MULT_COL_SPACER  0x40100000ul

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
    struct scroll_region region;
    int lines;
};

struct composed {
    wchar_t base;
    wchar_t combining[5];
    uint8_t count;
};

struct row {
    struct cell *cells;
    bool dirty;
    bool linebreak;
};

struct sixel {
    void *data;
    pixman_image_t *pix;
    int width;
    int height;
    int rows;
    int cols;
    struct coord pos;
};

struct grid {
    int num_rows;
    int num_cols;
    int offset;
    int view;

    struct cursor cursor;
    struct cursor saved_cursor;

    struct row **rows;
    struct row *cur_row;

    tll(struct damage) scroll_damage;
    tll(struct sixel) sixel_images;
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
    wchar_t utf8;
    struct {
        struct vt_param v[16];
        uint8_t idx;
    } params;
    uint32_t private; /* LSB=priv0, MSB=priv3 */
    struct {
        uint8_t *data;
        size_t size;
        size_t idx;
    } osc;
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
enum selection_semantic { SELECTION_SEMANTIC_NONE, SELECTION_SEMANTIC_WORD};
enum selection_direction {SELECTION_UNDIR, SELECTION_LEFT, SELECTION_RIGHT};
enum selection_scroll_direction {SELECTION_SCROLL_NOT, SELECTION_SCROLL_UP, SELECTION_SCROLL_DOWN};

struct ptmx_buffer {
    void *data;
    size_t len;
    size_t idx;
};

enum term_surface {
    TERM_SURF_NONE,
    TERM_SURF_GRID,
    TERM_SURF_SEARCH,
    TERM_SURF_SCROLLBACK_INDICATOR,
    TERM_SURF_RENDER_TIMER,
    TERM_SURF_TITLE,
    TERM_SURF_BORDER_LEFT,
    TERM_SURF_BORDER_RIGHT,
    TERM_SURF_BORDER_TOP,
    TERM_SURF_BORDER_BOTTOM,
    TERM_SURF_BUTTON_MINIMIZE,
    TERM_SURF_BUTTON_MAXIMIZE,
    TERM_SURF_BUTTON_CLOSE,
};

typedef tll(struct ptmx_buffer) ptmx_buffer_list_t;

struct terminal {
    struct fdm *fdm;
    struct reaper *reaper;
    const struct config *conf;

    pid_t slave;
    int ptmx;

    struct vt vt;
    struct grid *grid;
    struct grid normal;
    struct grid alt;

    int cols;   /* number of columns */
    int rows;   /* number of rows */
    struct scroll_region scroll_region;

    struct charsets charsets;
    struct charsets saved_charsets; /* For save/restore cursor + attributes */

    bool auto_margin;
    bool insert_mode;
    bool reverse;
    bool hide_cursor;
    bool reverse_wrap;
    bool bracketed_paste;
    bool focus_events;
    bool alt_scrolling;
    bool modify_escape_key;
    enum cursor_origin origin;
    enum cursor_keys cursor_keys_mode;
    enum keypad_keys keypad_keys_mode;
    enum mouse_tracking mouse_tracking;
    enum mouse_reporting mouse_reporting;

    tll(int) tab_stops;

    size_t composed_count;
    struct composed *composed;

    /* Temporary: for FDM */
    struct {
        bool is_armed;
        int lower_fd;
        int upper_fd;
    } delayed_render_timer;

    struct fcft_font *fonts[4];
    struct config_font *font_sizes[4];
    float font_dpi;
    int font_scale;
    enum fcft_subpixel font_subpixel;

    /*
     *   0-159: U+250U+259F
     * 160-219: U+1FB00-1FB3B
     */
    struct fcft_glyph *box_drawing[220];

    bool is_sending_paste_data;
    ptmx_buffer_list_t ptmx_buffers;
    ptmx_buffer_list_t ptmx_paste_buffers;

    struct {
        bool esc_prefix;
        bool eight_bit;
    } meta;

    bool num_lock_modifier;
    bool bell_action_enabled;

    /* Saved DECSET modes - we save the SET state */
    struct {
        uint32_t origin:1;
        uint32_t application_cursor_keys:1;
        uint32_t reverse:1;
        uint32_t show_cursor:1;
        uint32_t reverse_wrap:1;
        uint32_t auto_margin:1;
        uint32_t cursor_blink:1;
        uint32_t bracketed_paste:1;
        uint32_t focus_events:1;
        uint32_t alt_scrolling:1;
        //uint32_t mouse_x10:1;
        uint32_t mouse_click:1;
        uint32_t mouse_drag:1;
        uint32_t mouse_motion:1;
        //uint32_t mouse_utf8:1;
        uint32_t mouse_sgr:1;
        uint32_t mouse_urxvt:1;
        uint32_t meta_eight_bit:1;
        uint32_t meta_esc_prefix:1;
        uint32_t num_lock_modifier:1;
        uint32_t bell_action_enabled:1;
        uint32_t alt_screen:1;
        uint32_t modify_escape_key:1;
        uint32_t ime:1;
    } xtsave;

    char *window_title;
    tll(char *) window_title_stack;

    struct {
        bool active;
        int fd;
    } flash;

    struct {
        enum { BLINK_ON, BLINK_OFF } state;
        int fd;
    } blink;

    int scale;
    int width;  /* pixels */
    int height; /* pixels */
    int stashed_width;
    int stashed_height;
    struct {
        int left;
        int right;
        int top;
        int bottom;
    } margins;
    int cell_width;  /* pixels per cell, x-wise */
    int cell_height; /* pixels per cell, y-wise */

    struct {
        uint32_t fg;
        uint32_t bg;
        uint32_t table[256];
        uint16_t alpha;

        uint32_t default_fg;
        uint32_t default_bg;
        uint32_t default_table[256];
    } colors;

    enum cursor_style cursor_style;
    struct {
        bool decset;   /* Blink enabled via '\E[?12h' */
        bool deccsusr; /* Blink enabled via '\E[X q' */
        int fd;
        enum { CURSOR_BLINK_ON, CURSOR_BLINK_OFF } state;
    } cursor_blink;
    struct {
        uint32_t text;
        uint32_t cursor;
    } cursor_color;

    struct {
        enum selection_kind kind;
        enum selection_semantic semantic;
        enum selection_direction direction;
        struct coord start;
        struct coord end;
        bool ongoing;

        struct {
            int fd;
            int col;
            enum selection_scroll_direction direction;
        } auto_scroll;
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

    struct wayland *wl;
    struct wl_window *window;
    bool visual_focus;
    bool kbd_focus;
    enum term_surface active_surface;

    struct {
        /* Scheduled for rendering, as soon-as-possible */
        struct {
            bool grid;
            bool csd;
            bool search;
            bool title;
        } refresh;

        /* Scheduled for rendering, in the next frame callback */
        struct {
            bool grid;
            bool csd;
            bool search;
            bool title;
        } pending;

        bool margins;  /* Someone explicitly requested a refresh of the margins */
        bool urgency;  /* Signal 'urgency' (paint borders red) */

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
            mtx_t lock;
            tll(int) queue;
            thrd_t *threads;
            struct buffer *buf;
        } workers;

        /* Last rendered cursor position */
        struct {
            struct row *row;
            int col;
            bool hidden;
        } last_cursor;

        struct buffer *last_buf;     /* Buffer we rendered to last time */
        bool was_flashing;           /* Flash was active last time we rendered */
        bool was_searching;

        size_t search_glyph_offset;

        bool presentation_timings;
        struct timespec input_time;
    } render;

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
            bool autosize;
        } image;

        unsigned params[5];  /* Collected parameters, for RASTER, COLOR_SPEC */
        unsigned param;      /* Currently collecting parameter, for RASTER, COLOR_SPEC and REPEAT */
        unsigned param_idx;  /* Parameters seen */

        /* Application configurable */
        unsigned palette_size;  /* Number of colors in palette */
        unsigned max_width;     /* Maximum image width, in pixels */
        unsigned max_height;    /* Maximum image height, in pixels */
    } sixel;

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    struct {
        bool enabled;
        struct {
            wchar_t *text;
            struct cell *cells;
            int count;

            struct {
                bool hidden;
                int start;  /* Cell index, inclusive */
                int end;    /* Cell index, exclusive */
            } cursor;
        } preedit;
    } ime;
#endif

    bool quit;
    bool is_shutting_down;
    void (*shutdown_cb)(void *data, int exit_code);
    void *shutdown_data;

    char *foot_exe;
    char *cwd;
};

extern const char *const XCURSOR_HIDDEN;
extern const char *const XCURSOR_LEFT_PTR;
extern const char *const XCURSOR_TEXT;
//extern const char *const XCURSOR_HAND2;
extern const char *const XCURSOR_TOP_LEFT_CORNER;
extern const char *const XCURSOR_TOP_RIGHT_CORNER;
extern const char *const XCURSOR_BOTTOM_LEFT_CORNER;
extern const char *const XCURSOR_BOTTOM_RIGHT_CORNER;
extern const char *const XCURSOR_LEFT_SIDE;
extern const char *const XCURSOR_RIGHT_SIDE;
extern const char *const XCURSOR_TOP_SIDE;
extern const char *const XCURSOR_BOTTOM_SIDE;

struct config;
struct terminal *term_init(
    const struct config *conf, struct fdm *fdm, struct reaper *reaper,
    struct wayland *wayl, const char *foot_exe, const char *cwd,
    int argc, char *const *argv,
    void (*shutdown_cb)(void *data, int exit_code), void *shutdown_data);

bool term_shutdown(struct terminal *term);
int term_destroy(struct terminal *term);

void term_reset(struct terminal *term, bool hard);
bool term_to_slave(struct terminal *term, const void *data, size_t len);
bool term_paste_data_to_slave(
    struct terminal *term, const void *data, size_t len);

bool term_font_size_increase(struct terminal *term);
bool term_font_size_decrease(struct terminal *term);
bool term_font_size_reset(struct terminal *term);
bool term_font_dpi_changed(struct terminal *term);
void term_font_subpixel_changed(struct terminal *term);

void term_window_configured(struct terminal *term);

void term_damage_rows(struct terminal *term, int start, int end);
void term_damage_rows_in_view(struct terminal *term, int start, int end);

void term_damage_all(struct terminal *term);
void term_damage_view(struct terminal *term);

void term_damage_cursor(struct terminal *term);
void term_damage_margins(struct terminal *term);

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
void term_cursor_blink_update(struct terminal *term);

void term_print(struct terminal *term, wchar_t wc, int width);

void term_scroll(struct terminal *term, int rows);
void term_scroll_reverse(struct terminal *term, int rows);

void term_scroll_partial(
    struct terminal *term, struct scroll_region region, int rows);
void term_scroll_reverse_partial(
    struct terminal *term, struct scroll_region region, int rows);

void term_carriage_return(struct terminal *term);
void term_linefeed(struct terminal *term);
void term_reverse_index(struct terminal *term);

void term_arm_blink_timer(struct terminal *term);

void term_restore_cursor(struct terminal *term, const struct cursor *cursor);

void term_visual_focus_in(struct terminal *term);
void term_visual_focus_out(struct terminal *term);
void term_kbd_focus_in(struct terminal *term);
void term_kbd_focus_out(struct terminal *term);
void term_mouse_down(
    struct terminal *term, int button, int row, int col,
    bool shift, bool alt, bool ctrl);
void term_mouse_up(
    struct terminal *term, int button, int row, int col,
    bool shift, bool alt, bool ctrl);
void term_mouse_motion(
    struct terminal *term, int button, int row, int col,
    bool shift, bool alt, bool ctrl);
bool term_mouse_grabbed(const struct terminal *term, struct seat *seat);
void term_xcursor_update(struct terminal *term);
void term_xcursor_update_for_seat(struct terminal *term, struct seat *seat);

void term_set_window_title(struct terminal *term, const char *title);
void term_flash(struct terminal *term, unsigned duration_ms);
void term_bell(struct terminal *term);
bool term_spawn_new(const struct terminal *term);

void term_enable_app_sync_updates(struct terminal *term);
void term_disable_app_sync_updates(struct terminal *term);

enum term_surface term_surface_kind(
    const struct terminal *term, const struct wl_surface *surface);

bool term_scrollback_to_text(
    const struct terminal *term, char **text, size_t *len);
bool term_view_to_text(
    const struct terminal *term, char **text, size_t *len);

bool term_ime_is_enabled(const struct terminal *term);
void term_ime_enable(struct terminal *term);
void term_ime_disable(struct terminal *term);
void term_ime_reset(struct terminal *term);
