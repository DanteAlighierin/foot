#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <threads.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "tllist.h"

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

    tll(struct damage) damage;
    tll(struct damage) scroll_damage;
};

struct vt_subparams {
    unsigned value[16];
    int idx;
};

struct vt_param {
    unsigned value;
    struct vt_subparams sub;
};

struct vt {
    int state;  /* enum state */
    struct {
        struct vt_param v[16];
        int idx;
    } params;
    struct {
        uint8_t data[2];
        int idx;
    } intermediates;
    struct {
        uint8_t data[1024];
        int idx;
    } osc;
    struct {
        uint8_t data[4];
        int idx;
        int left;
    } utf8;
    struct attributes attrs;
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

struct terminal {
    pid_t slave;
    int ptmx;

    enum decckm decckm;
    enum keypad_mode keypad_mode;
    bool bracketed_paste;

    struct vt vt;
    struct kbd kbd;

    int cols;
    int rows;
    int cell_width;
    int cell_height;

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
