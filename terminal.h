#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <threads.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "tllist.h"

struct attributes {
    bool bold;
    bool italic;
    bool underline;
    bool strikethrough;
    bool blink;
    bool conceal;
    bool reverse;
    uint32_t foreground;
    uint32_t background;
};

struct cell {
    char c[5];
    struct attributes attrs;
};

enum damage_type {DAMAGE_UPDATE, DAMAGE_ERASE, DAMAGE_SCROLL};
struct damage {
    enum damage_type type;
    union {
        /* DAMAGE_UPDATE, DAMAGE_ERASE */
        struct {
            int start;
            int length;
        } range;

        /* DAMAGE_SCROLL */
        struct {
            int top_margin;
            int bottom_margin;
            int lines;
        } scroll;
    };
};

struct grid {
    int cols;
    int rows;
    int cell_width;
    int cell_height;

    int linear_cursor;
    struct {
        int row;
        int col;
    } cursor;
    bool print_needs_wrap;

    struct cell *cells;
    struct cell *normal_grid;
    struct cell *alt_grid;
    struct {
        int row;
        int col;
    } alt_saved_cursor;

    uint32_t foreground;
    uint32_t background;

    tll(struct damage) damage;
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
    struct grid grid;
    struct kbd kbd;
};
