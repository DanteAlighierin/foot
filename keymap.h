#pragma once

#include <xkbcommon/xkbcommon.h>

#include "terminal.h"

enum modifier {
    MOD_NONE = 0x0,
    MOD_ANY = 0x1,
    MOD_SHIFT = 0x2,
    MOD_ALT = 0x4,
    MOD_CTRL = 0x8,
    MOD_META = 0x10,
    MOD_MODIFY_OTHER_KEYS_STATE1 = 0x20,
    MOD_MODIFY_OTHER_KEYS_STATE2 = 0x40,
};

struct key_data {
    enum modifier modifiers;
    enum cursor_keys cursor_keys_mode;
    enum keypad_keys keypad_keys_mode;
    const char *seq;
};

static const struct key_data key_escape[] = {
    {MOD_SHIFT,                                 CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;2;27~"},
    {MOD_ALT,                                   CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\033"},
    {MOD_SHIFT | MOD_ALT,                       CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;4;27~"},
    {MOD_CTRL,                                  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;5;27~"},
    {MOD_SHIFT | MOD_CTRL,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;6;27~"},
    {MOD_ALT | MOD_CTRL,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;7;27~"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;8;27~"},
    {MOD_META,                                  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;9;27~"},
    {MOD_META | MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;10;27~"},
    {MOD_META | MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;11;27~"},
    {MOD_META | MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;12;27~"},
    {MOD_META | MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;13;27~"},
    {MOD_META | MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;14;27~"},
    {MOD_META | MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;15;27~"},
    {MOD_META | MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;16;27~"},
    {MOD_ANY,   CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033"},
};

static const struct key_data key_return[] = {
    {MOD_SHIFT,                                 CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;2;13~"},
    {MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE1,    CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\r"},
    {MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE2,    CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;3;13~"},
    {MOD_SHIFT | MOD_ALT,                       CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;4;13~"},
    {MOD_CTRL,                                  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;5;13~"},
    {MOD_SHIFT | MOD_CTRL,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;6;13~"},
    {MOD_ALT | MOD_CTRL,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;7;13~"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;8;13~"},
    {MOD_META,                                  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;9;13~"},
    {MOD_META | MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;10;13~"},
    {MOD_META | MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;11;13~"},
    {MOD_META | MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;12;13~"},
    {MOD_META | MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;13;13~"},
    {MOD_META | MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;14;13~"},
    {MOD_META | MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;15;13~"},
    {MOD_META | MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;16;13~"},
    {MOD_ANY,                                   CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\r"},
};

/* Tab isn't covered by the regular "modifyOtherKeys" handling */
static const struct key_data key_tab[] = {
    {MOD_SHIFT | MOD_MODIFY_OTHER_KEYS_STATE1,  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[Z"},
    {MOD_SHIFT | MOD_MODIFY_OTHER_KEYS_STATE2,  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;2;9~"},
    {MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE1,    CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\t"},
    {MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE2,    CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;3;9~"},
    {MOD_SHIFT | MOD_ALT,                       CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;4;9~"},
    {MOD_CTRL,                                  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;5;9~"},
    {MOD_SHIFT | MOD_CTRL,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;6;9~"},
    {MOD_ALT | MOD_CTRL,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;7;9~"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;8;9~"},
    {MOD_META,                                  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;9;9~"},
    {MOD_META | MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;10;9~"},
    {MOD_META | MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;11;9~"},
    {MOD_META | MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;12;9~"},
    {MOD_META | MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;13;9~"},
    {MOD_META | MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;14;9~"},
    {MOD_META | MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;15;9~"},
    {MOD_META | MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;16;9~"},
    {MOD_ANY,                                   CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\t"},
};

/*
 * Shift+Tab produces ISO_Left_Tab
 *
 * However, all combos (except Shift+Tab) acts as if we pressed
 * mods+shift+tab.
 */
static const struct key_data key_iso_left_tab[] = {
    {MOD_SHIFT | MOD_ALT,                       CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;4;9~"},
    {MOD_SHIFT | MOD_CTRL,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;6;9~"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;8;9~"},
    {MOD_SHIFT | MOD_META,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;10;9~"},
    {MOD_SHIFT | MOD_META | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;12;9~"},
    {MOD_SHIFT | MOD_META | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;14;9~"},
    {MOD_SHIFT | MOD_META | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;16;9~"},
    {MOD_ANY,                                   CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[Z"},
};

static const struct key_data key_backspace[] = {
    {MOD_SHIFT | MOD_MODIFY_OTHER_KEYS_STATE1,                                 CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\x7f"},
    {MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE1,                                   CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\x7f"},
    {MOD_SHIFT | MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE1,                       CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\x7f"},
    {MOD_SHIFT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE1,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\x08"},
    {MOD_ALT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE1,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\x08"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE1,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\x08"},
    {MOD_META | MOD_MODIFY_OTHER_KEYS_STATE1,                                  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\x7f"},
    {MOD_META | MOD_SHIFT | MOD_MODIFY_OTHER_KEYS_STATE1,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\x7f"},
    {MOD_META | MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE1,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\x7f"},
    {MOD_META | MOD_SHIFT | MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE1,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\x7f"},
    {MOD_META | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE1,                       CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\x08"},
    {MOD_META | MOD_SHIFT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE1,           CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\x08"},
    {MOD_META | MOD_ALT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE1,             CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\x08"},
    {MOD_META | MOD_SHIFT | MOD_ALT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE1, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\x08"},

    {MOD_SHIFT | MOD_MODIFY_OTHER_KEYS_STATE2,                                 CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;2;127~"},
    {MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE2,                                   CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;3;127~"},
    {MOD_SHIFT | MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE2,                       CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;4;127~"},
    {MOD_SHIFT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE2,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;6;8~"},
    {MOD_ALT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE2,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;7;8~"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE2,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;8;8~"},
    {MOD_META | MOD_MODIFY_OTHER_KEYS_STATE2,                                  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;9;127~"},
    {MOD_META | MOD_SHIFT | MOD_MODIFY_OTHER_KEYS_STATE2,                      CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;10;127~"},
    {MOD_META | MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE2,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;11;127~"},
    {MOD_META | MOD_SHIFT | MOD_ALT | MOD_MODIFY_OTHER_KEYS_STATE2,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;12;127~"},
    {MOD_META | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE2,                       CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;13;8~"},
    {MOD_META | MOD_SHIFT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE2,           CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;14;8~"},
    {MOD_META | MOD_ALT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE2,             CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;15;8~"},
    {MOD_META | MOD_SHIFT | MOD_ALT | MOD_CTRL | MOD_MODIFY_OTHER_KEYS_STATE2, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;16;8~"},

    {MOD_CTRL,                                                                 CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\x08"},
    {MOD_ANY,                                                                  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\x7f"},
};

#define DEFAULT_MODS_FOR_SINGLE(sym)                                    \
    {MOD_SHIFT,                                 CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;2"#sym}, \
    {MOD_ALT,                                   CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;3"#sym}, \
    {MOD_SHIFT | MOD_ALT,                       CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;4"#sym}, \
    {MOD_CTRL,                                  CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;5"#sym}, \
    {MOD_SHIFT | MOD_CTRL,                      CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;6"#sym}, \
    {MOD_ALT | MOD_CTRL,                        CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;7"#sym}, \
    {MOD_SHIFT | MOD_ALT | MOD_CTRL,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;8"#sym}, \
    {MOD_META,                                  CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;9"#sym}, \
    {MOD_META | MOD_SHIFT,                      CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;10"#sym}, \
    {MOD_META | MOD_ALT,                        CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;11"#sym}, \
    {MOD_META | MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;12"#sym}, \
    {MOD_META | MOD_CTRL,                       CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;13"#sym}, \
    {MOD_META | MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;14"#sym}, \
    {MOD_META | MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;15"#sym}, \
    {MOD_META | MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;16"#sym}

#define DEFAULT_MODS_FOR_TILDE(sym) \
    {MOD_SHIFT,                                 CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";2~"}, \
    {MOD_ALT,                                   CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";3~"}, \
    {MOD_SHIFT | MOD_ALT,                       CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";4~"}, \
    {MOD_CTRL,                                  CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";5~"}, \
    {MOD_SHIFT | MOD_CTRL,                      CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";6~"}, \
    {MOD_ALT | MOD_CTRL,                        CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";7~"}, \
    {MOD_SHIFT | MOD_ALT | MOD_CTRL,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";8~"}, \
    {MOD_META,                                  CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";9~"}, \
    {MOD_META | MOD_SHIFT,                      CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";10~"}, \
    {MOD_META  | MOD_ALT,                       CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";11~"}, \
    {MOD_META | MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";12~"}, \
    {MOD_META  | MOD_CTRL,                      CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";13~"}, \
    {MOD_META | MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";14~"}, \
    {MOD_META  | MOD_ALT | MOD_CTRL,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";15~"}, \
    {MOD_META | MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";16~"}


static const struct key_data key_up[] = {
    DEFAULT_MODS_FOR_SINGLE(A),
    {MOD_ANY,                        CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OA"},
    {MOD_ANY,                        CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[A"},

};

static const struct key_data key_down[] = {
    DEFAULT_MODS_FOR_SINGLE(B),
    {MOD_ANY,                        CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OB"},
    {MOD_ANY,                        CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[B"},
};

static const struct key_data key_right[] = {
    DEFAULT_MODS_FOR_SINGLE(C),
    {MOD_ANY,                        CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OC"},
    {MOD_ANY,                        CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[C"},
};

static const struct key_data key_left[] = {
    DEFAULT_MODS_FOR_SINGLE(D),
    {MOD_ANY,                        CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OD"},
    {MOD_ANY,                        CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[D"},
};

static const struct key_data key_home[] = {
    DEFAULT_MODS_FOR_SINGLE(H),
    {MOD_ANY,                        CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OH"},
    {MOD_ANY,                        CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[H"},
};

static const struct key_data key_end[] = {
    DEFAULT_MODS_FOR_SINGLE(F),
    {MOD_ANY,                        CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OF"},
    {MOD_ANY,                        CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[F"},
};

static const struct key_data key_insert[] = {
    DEFAULT_MODS_FOR_TILDE(2),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[2~"},
};

static const struct key_data key_delete[] = {
    DEFAULT_MODS_FOR_TILDE(3),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[3~"},
};

static const struct key_data key_pageup[] = {
    DEFAULT_MODS_FOR_TILDE(5),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[5~"},
};

static const struct key_data key_pagedown[] = {
    DEFAULT_MODS_FOR_TILDE(6),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[6~"},
};

static const struct key_data key_f1[] = {
    DEFAULT_MODS_FOR_SINGLE(P),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033OP"},
};

static const struct key_data key_f2[] = {
    DEFAULT_MODS_FOR_SINGLE(Q),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033OQ"},
};

static const struct key_data key_f3[] = {
    DEFAULT_MODS_FOR_SINGLE(R),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033OR"},
};

static const struct key_data key_f4[] = {
    DEFAULT_MODS_FOR_SINGLE(S),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033OS"},
};

static const struct key_data key_f5[] = {
    DEFAULT_MODS_FOR_TILDE(15),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[15~"},
};

static const struct key_data key_f6[] = {
    DEFAULT_MODS_FOR_TILDE(17),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[17~"},
};
static const struct key_data key_f7[] = {
    DEFAULT_MODS_FOR_TILDE(18),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[18~"},
};

static const struct key_data key_f8[] = {
    DEFAULT_MODS_FOR_TILDE(19),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[19~"},
};

static const struct key_data key_f9[] = {
    DEFAULT_MODS_FOR_TILDE(20),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[20~"},
};

static const struct key_data key_f10[] = {
    DEFAULT_MODS_FOR_TILDE(21),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[21~"},
};

static const struct key_data key_f11[] = {
    DEFAULT_MODS_FOR_TILDE(23),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[23~"},
};

static const struct key_data key_f12[] = {
    DEFAULT_MODS_FOR_TILDE(24),
    {MOD_ANY,                        CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[24~"},
};

static const struct key_data key_f13[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[1;2P"}};
static const struct key_data key_f14[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[1;2Q"}};
static const struct key_data key_f15[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[1;2R"}};
static const struct key_data key_f16[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[1;2S"}};
static const struct key_data key_f17[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[15;2~"}};
static const struct key_data key_f18[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[17;2~"}};
static const struct key_data key_f19[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[18;2~"}};
static const struct key_data key_f20[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[19;2~"}};
static const struct key_data key_f21[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[20;2~"}};
static const struct key_data key_f22[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[21;2~"}};
static const struct key_data key_f23[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[23;2~"}};
static const struct key_data key_f24[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[24;2~"}};
static const struct key_data key_f25[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[1;5P"}};
static const struct key_data key_f26[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[1;5Q"}};
static const struct key_data key_f27[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[1;5R"}};
static const struct key_data key_f28[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[1;5S"}};
static const struct key_data key_f29[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[15;5~"}};
static const struct key_data key_f30[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[17;5~"}};
static const struct key_data key_f31[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[18;5~"}};
static const struct key_data key_f32[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[19;5~"}};
static const struct key_data key_f33[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[20;5~"}};
static const struct key_data key_f34[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[21;5~"}};
static const struct key_data key_f35[] = {{MOD_NONE, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[23;5~"}};

/* Keypad keys don't map shift */
#undef DEFAULT_MODS_FOR_SINGLE
#undef DEFAULT_MODS_FOR_TILDE

#define DEFAULT_MODS_FOR_SINGLE(sym)                                    \
    {MOD_ALT,                                   CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;3"#sym}, \
    {MOD_CTRL,                                  CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;5"#sym}, \
    {MOD_ALT | MOD_CTRL,                        CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;7"#sym}, \
    {MOD_META,                                  CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;9"#sym}, \
    {MOD_META | MOD_ALT,                        CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;11"#sym}, \
    {MOD_META | MOD_CTRL,                       CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;13"#sym}, \
    {MOD_META | MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;15"#sym}

#define DEFAULT_MODS_FOR_TILDE(sym) \
    {MOD_ALT,                                   CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";3~"}, \
    {MOD_CTRL,                                  CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";5~"}, \
    {MOD_ALT | MOD_CTRL,                        CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";7~"}, \
    {MOD_META,                                  CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";9~"}, \
    {MOD_META  | MOD_ALT,                       CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";11~"}, \
    {MOD_META  | MOD_CTRL,                      CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";13~"}, \
    {MOD_META  | MOD_ALT | MOD_CTRL,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033["#sym";15~"}

static const struct key_data key_kp_up[] = {
    DEFAULT_MODS_FOR_SINGLE(A),
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[A"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OA"},
};

static const struct key_data key_kp_down[] = {
    DEFAULT_MODS_FOR_SINGLE(B),
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[B"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OB"},
};

static const struct key_data key_kp_right[] = {
    DEFAULT_MODS_FOR_SINGLE(C),
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[C"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OC"},
};

static const struct key_data key_kp_left[] = {
    DEFAULT_MODS_FOR_SINGLE(D),
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[D"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OD"},
};

static const struct key_data key_kp_begin[] = {
    DEFAULT_MODS_FOR_SINGLE(E),
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[E"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OE"},
};

static const struct key_data key_kp_home[] = {
    DEFAULT_MODS_FOR_SINGLE(H),
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[H"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OH"},
};

static const struct key_data key_kp_end[] = {
    DEFAULT_MODS_FOR_SINGLE(F),
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[F"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OF"},
};

static const struct key_data key_kp_insert[] = {
    DEFAULT_MODS_FOR_TILDE(2),
    {MOD_ANY,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[2~"},
};

static const struct key_data key_kp_delete[] = {
    DEFAULT_MODS_FOR_TILDE(3),
    {MOD_ANY,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[3~"},
};

static const struct key_data key_kp_pageup[] = {
    DEFAULT_MODS_FOR_TILDE(5),
    {MOD_ANY,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[5~"},
};

static const struct key_data key_kp_pagedown[] = {
    DEFAULT_MODS_FOR_TILDE(6),
    {MOD_ANY,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[6~"},
};

#undef DEFAULT_MODS_FOR_SINGLE
#undef DEFAULT_MODS_FOR_TILDE

#define DEFAULT_MODS_FOR_KP(sym) \
    {MOD_NONE,                                  CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O"#sym}, \
    {MOD_SHIFT,                                 CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2"#sym}, \
    {MOD_ALT,                                   CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3"#sym}, \
    {MOD_SHIFT | MOD_ALT,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4"#sym}, \
    {MOD_CTRL,                                  CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5"#sym}, \
    {MOD_SHIFT | MOD_CTRL,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6"#sym}, \
    {MOD_ALT | MOD_CTRL,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7"#sym}, \
    {MOD_SHIFT | MOD_ALT | MOD_CTRL,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8"#sym}, \
    {MOD_META,                                  CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O9"#sym}, \
    {MOD_META | MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O10"#sym}, \
    {MOD_META  | MOD_ALT,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O11"#sym}, \
    {MOD_META | MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O12"#sym}, \
    {MOD_META  | MOD_CTRL,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O13"#sym}, \
    {MOD_META | MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O14"#sym}, \
    {MOD_META  | MOD_ALT | MOD_CTRL,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O15"#sym}, \
    {MOD_META | MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O16"#sym}

static const struct key_data key_kp_enter[] = {DEFAULT_MODS_FOR_KP(M)};
static const struct key_data key_kp_divide[] = {DEFAULT_MODS_FOR_KP(o)};
static const struct key_data key_kp_multiply[] = {DEFAULT_MODS_FOR_KP(j)};
static const struct key_data key_kp_subtract[] = {DEFAULT_MODS_FOR_KP(m)};
static const struct key_data key_kp_add[] = {DEFAULT_MODS_FOR_KP(k)};
static const struct key_data key_kp_separator[] = {DEFAULT_MODS_FOR_KP(l)};
static const struct key_data key_kp_decimal[] = {DEFAULT_MODS_FOR_KP(n)};
static const struct key_data key_kp_0[] = {DEFAULT_MODS_FOR_KP(p)};
static const struct key_data key_kp_1[] = {DEFAULT_MODS_FOR_KP(q)};
static const struct key_data key_kp_2[] = {DEFAULT_MODS_FOR_KP(r)};
static const struct key_data key_kp_3[] = {DEFAULT_MODS_FOR_KP(s)};
static const struct key_data key_kp_4[] = {DEFAULT_MODS_FOR_KP(t)};
static const struct key_data key_kp_5[] = {DEFAULT_MODS_FOR_KP(u)};
static const struct key_data key_kp_6[] = {DEFAULT_MODS_FOR_KP(v)};
static const struct key_data key_kp_7[] = {DEFAULT_MODS_FOR_KP(w)};
static const struct key_data key_kp_8[] = {DEFAULT_MODS_FOR_KP(x)};
static const struct key_data key_kp_9[] = {DEFAULT_MODS_FOR_KP(y)};

#undef DEFAULT_MODS_FOR_KP
