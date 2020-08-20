#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#include <threads.h>
#include <semaphore.h>

#if defined(FOOT_GRAPHEME_CLUSTERING)
 #include <utf8proc.h>
#endif

#include <tllist.h>
#include <fcft/fcft.h>

//#include "config.h"
#include "debug.h"
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
    bool bold:1;
    bool dim:1;
    bool italic:1;
    bool underline:1;
    bool strikethrough:1;
    bool blink:1;
    bool conceal:1;
    bool reverse:1;
    uint32_t fg:24;

    bool clean:1;
    bool have_fg:1;
    bool have_bg:1;
    uint32_t selected:2;
    bool url:1;
    uint32_t reserved:2;
    uint32_t bg:24;
};
static_assert(sizeof(struct attributes) == 8, "VT attribute struct too large");

#define CELL_COMB_CHARS_LO    0x40000000ul
#define CELL_COMB_CHARS_HI    0x400ffffful
#define CELL_SPACER           0x40100000ul

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
    wchar_t chars[20];
    uint8_t count;
};

struct row_uri_range {
    int start;
    int end;
    uint64_t id;
    char *uri;
};

struct row_data {
    tll(struct row_uri_range) uri_ranges;
};

struct row {
    struct cell *cells;
    bool dirty;
    bool linebreak;
    struct row_data *extra;
};

struct sixel {
    void *data;
    pixman_image_t *pix;
    int width;
    int height;
    int rows;
    int cols;
    struct coord pos;
    bool opaque;
};

struct grid {
    int num_rows;
    int num_cols;
    int offset;
    int view;

    /*
     * Note: the cursor (not the *saved* cursor) could most likely be
     * global state in the term struct.
     *
     * However, we have grid specific functions that does not have
     * access to the owning term struct, but does need access to the
     * cursor.
     */
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
#if defined(FOOT_GRAPHEME_CLUSTERING)
    utf8proc_int32_t grapheme_state;
#endif
    wchar_t utf8;
    struct {
        struct vt_param v[16];
        uint8_t idx;
    } params;

    uint32_t private; /* LSB=priv0, MSB=priv3 */

    struct attributes attrs;
    struct attributes saved_attrs;

    struct {
        uint8_t *data;
        size_t size;
        size_t idx;
    } osc;

    /* Start coordinate for current OSC-8 URI */
    struct {
        uint64_t id;
        char *uri;
        struct coord begin;
    } osc8;

    struct {
        uint8_t *data;
        size_t size;
        size_t idx;
        void (*put_handler)(struct terminal *term, uint8_t c);
        void (*unhook_handler)(struct terminal *term);
    } dcs;
};

enum cursor_origin { ORIGIN_ABSOLUTE, ORIGIN_RELATIVE };
enum cursor_keys { CURSOR_KEYS_DONTCARE, CURSOR_KEYS_NORMAL, CURSOR_KEYS_APPLICATION };
enum keypad_keys { KEYPAD_DONTCARE, KEYPAD_NUMERICAL, KEYPAD_APPLICATION };
enum charset { CHARSET_ASCII, CHARSET_GRAPHIC };
enum charset_designator { G0, G1, G2, G3 };

struct charsets {
    enum charset_designator selected;
    enum charset_designator saved;
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

enum cursor_style { CURSOR_BLOCK, CURSOR_UNDERLINE, CURSOR_BEAM };

enum selection_kind {
    SELECTION_NONE,
    SELECTION_CHAR_WISE,
    SELECTION_WORD_WISE,
    SELECTION_LINE_WISE,
    SELECTION_BLOCK
};
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
    TERM_SURF_JUMP_LABEL,
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

enum url_action { URL_ACTION_COPY, URL_ACTION_LAUNCH };
struct url {
    uint64_t id;
    char *url;
    wchar_t *key;
    struct coord start;
    struct coord end;
    enum url_action action;
    bool url_mode_dont_change_url_attr; /* Entering/exiting URL mode doesn’t touch the cells’ attr.url */
    bool osc8;
};
typedef tll(struct url) url_list_t;

/* If px != 0 then px is valid, otherwise pt is valid */
struct pt_or_px {
    int16_t px;
    float pt;
};

struct terminal {
    struct fdm *fdm;
    struct reaper *reaper;
    const struct config *conf;

    void (*ascii_printer)(struct terminal *term, wchar_t c);

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
    struct pt_or_px font_line_height;
    float font_dpi;
    int16_t font_x_ofs;
    int16_t font_y_ofs;
    enum fcft_subpixel font_subpixel;

    /*
     *   0-159: U+250U+259F
     * 160-219: U+1FB00-1FB3B
     * 220-247: U+1FB70-1FB8B
     */
    struct fcft_glyph *box_drawing[248];

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
        bool origin:1;
        bool application_cursor_keys:1;
        bool reverse:1;
        bool show_cursor:1;
        bool reverse_wrap:1;
        bool auto_margin:1;
        bool cursor_blink:1;
        bool bracketed_paste:1;
        bool focus_events:1;
        bool alt_scrolling:1;
        //bool mouse_x10:1;
        bool mouse_click:1;
        bool mouse_drag:1;
        bool mouse_motion:1;
        //bool mouse_utf8:1;
        bool mouse_sgr:1;
        bool mouse_urxvt:1;
        bool meta_eight_bit:1;
        bool meta_esc_prefix:1;
        bool num_lock_modifier:1;
        bool bell_action_enabled:1;
        bool alt_screen:1;
        bool modify_escape_key:1;
        bool ime:1;
        bool app_sync_updates:1;

        bool sixel_scrolling:1;
        bool sixel_private_palette:1;
        bool sixel_cursor_right_of_graphics:1;
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
        uint32_t selection_fg;
        uint32_t selection_bg;
        bool use_custom_selection;
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
        enum selection_direction direction;
        struct coord start;
        struct coord end;
        bool ongoing;
        bool spaces_only; /* SELECTION_SEMANTIC_WORD */

        struct {
            struct coord start;
            struct coord end;
        } pivot;

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
            bool urls;
        } refresh;

        /* Scheduled for rendering, in the next frame callback */
        struct {
            bool grid;
            bool csd;
            bool search;
            bool urls;
        } pending;

        bool margins;  /* Someone explicitly requested a refresh of the margins */
        bool urgency;  /* Signal 'urgency' (paint borders red) */

        struct {
            struct timeval last_update;
            bool is_armed;
            int timer_fd;
        } title;

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
        int max_non_empty_row_no;
        size_t row_byte_ofs; /* Byte position into image, for current row */
        int color_idx;       /* Current palette index */
        uint32_t *private_palette;   /* Private palette, used when private mode 1070 is enabled */
        uint32_t *shared_palette;    /* Shared palette, used when private mode 1070 is disabled */
        uint32_t *palette;   /* Points to either private_palette or shared_palette */

        struct {
            uint32_t *data;  /* Raw image data, in ARGB */
            int width;       /* Image width, in pixels */
            int height;      /* Image height, in pixels */
        } image;

        bool scrolling:1;                 /* Private mode 80 */
        bool use_private_palette:1;       /* Private mode 1070 */
        bool cursor_right_of_graphics:1;  /* Private mode 8452 */

        unsigned params[5];  /* Collected parameters, for RASTER, COLOR_SPEC */
        unsigned param;      /* Currently collecting parameter, for RASTER, COLOR_SPEC and REPEAT */
        unsigned param_idx;  /* Parameters seen */

        bool transparent_bg;

        /* Application configurable */
        unsigned palette_size;  /* Number of colors in palette */
        unsigned max_width;     /* Maximum image width, in pixels */
        unsigned max_height;    /* Maximum image height, in pixels */
    } sixel;

    /* TODO: wrap in a struct */
    url_list_t urls;
    wchar_t url_keys[5];
    bool urls_show_uri_on_jump_label;
    struct grid *url_grid_snapshot;

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    bool ime_enabled;
#endif

    bool is_shutting_down;
    bool slave_has_been_reaped;
    int exit_status;
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

void term_update_ascii_printer(struct terminal *term);
void term_single_shift(struct terminal *term, enum charset_designator idx);

void term_reset(struct terminal *term, bool hard);
bool term_to_slave(struct terminal *term, const void *data, size_t len);
bool term_paste_data_to_slave(
    struct terminal *term, const void *data, size_t len);

bool term_font_size_increase(struct terminal *term);
bool term_font_size_decrease(struct terminal *term);
bool term_font_size_reset(struct terminal *term);
bool term_font_dpi_changed(struct terminal *term, int old_scale);
void term_font_subpixel_changed(struct terminal *term);
int term_pt_or_px_as_pixels(
    const struct terminal *term, const struct pt_or_px *pt_or_px);


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

void term_save_cursor(struct terminal *term);
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
bool term_ime_reset(struct terminal *term);
void term_ime_set_cursor_rect(
    struct terminal *term, int x, int y, int width, int height);

void term_urls_reset(struct terminal *term);
void term_collect_urls(struct terminal *term);

void term_osc8_open(struct terminal *term, uint64_t id, const char *uri);
void term_osc8_close(struct terminal *term);

static inline void term_reset_grapheme_state(struct terminal *term)
{
#if defined(FOOT_GRAPHEME_CLUSTERING)
    term->vt.grapheme_state = 0;
#endif
}
