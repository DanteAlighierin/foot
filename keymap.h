#pragma once

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>

#include "terminal.h"

enum modifier {
    MOD_NONE = 0x0,
    MOD_ANY = 0x1,
    MOD_SHIFT = 0x2,
    MOD_ALT = 0x4,
    MOD_CTRL = 0x8,
    MOD_META = 0x10,
};

struct key_data {
    enum modifier modifiers;
    enum cursor_keys cursor_keys_mode;
    enum keypad_keys keypad_keys_mode;
    const char *seq;
};

struct key_map {
    xkb_keysym_t sym;
    size_t count;
    const struct key_data *data;
};

static const struct key_data key_escape[] = {
    {MOD_ANY,   CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033"},
};

static const struct key_data key_return[] = {
    {MOD_ANY,   CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\r"},
};

static const struct key_data key_tab[] = {
    {MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[27;5;9~"},  /* TODO: this is my own hack... */
    {MOD_ANY,  CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\t"},
};

static const struct key_data key_backtab[] = {
    {MOD_ANY, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[Z"},
};

static const struct key_data key_backspace[] = {
    {MOD_ALT, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\x7f"},
    {MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033\x7f"},
    {MOD_ANY, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\x7f"},
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

#undef DEFAULT_MODS_FOR_SINGLE
#undef DEFAULT_MODS_FOR_TILDE

static const struct key_data key_kp_up[] = {
    {MOD_ALT,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;3A"},
    {MOD_CTRL,           CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;5A"},
    {MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;7A"},
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[A"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OA"},
};

static const struct key_data key_kp_down[] = {
    {MOD_ALT,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;3B"},
    {MOD_CTRL,           CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;5B"},
    {MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;7B"},
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[B"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OB"},
};

static const struct key_data key_kp_right[] = {
    {MOD_ALT,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;3C"},
    {MOD_CTRL,           CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;5C"},
    {MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;7C"},
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[C"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OC"},
};

static const struct key_data key_kp_left[] = {
    {MOD_ALT,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;3D"},
    {MOD_CTRL,           CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;5D"},
    {MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;7D"},
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[D"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OD"},
};

static const struct key_data key_kp_home[] = {
    {MOD_ALT,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;3H"},
    {MOD_CTRL,           CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;5H"},
    {MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;7H"},
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[H"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OH"},
};

static const struct key_data key_kp_end[] = {
    {MOD_ALT,            CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;3F"},
    {MOD_CTRL,           CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;5F"},
    {MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE,    KEYPAD_DONTCARE, "\033[1;7F"},
    {MOD_ANY,            CURSOR_KEYS_NORMAL,      KEYPAD_DONTCARE, "\033[F"},
    {MOD_ANY,            CURSOR_KEYS_APPLICATION, KEYPAD_DONTCARE, "\033OF"},
};

static const struct key_data key_kp_insert[] = {
    {MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[2;3~"},
    {MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[2;5~"},
    {MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[2;7~"},
    {MOD_ANY,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[2~"},
};

static const struct key_data key_kp_delete[] = {
    {MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[3;3~"},
    {MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[3;5~"},
    {MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[3;7~"},
    {MOD_ANY,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[3~"},
};

static const struct key_data key_kp_pageup[] = {
    {MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[5;3~"},
    {MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[5;5~"},
    {MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[5;7~"},
    {MOD_ANY,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[5~"},
};

static const struct key_data key_kp_pagedown[] = {
    {MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[5;3~"},
    {MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[5;5~"},
    {MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[5;7~"},
    {MOD_ANY,            CURSOR_KEYS_DONTCARE, KEYPAD_DONTCARE, "\033[6~"},
};

static const struct key_data key_kp_divide[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Oo"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2o"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3o"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4o"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5o"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6o"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7o"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8o"},
};

static const struct key_data key_kp_multiply[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Oj"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2j"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3j"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4j"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5j"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6j"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7j"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8j"},
};

static const struct key_data key_kp_subtract[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Om"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2m"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3m"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4m"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5m"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6m"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7m"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8m"},
};

static const struct key_data key_kp_add[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Ok"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2k"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3k"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4k"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5k"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6k"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7k"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8k"},
};

static const struct key_data key_kp_separator[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Ol"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2l"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3l"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4l"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5l"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6l"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7l"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8l"},
};

static const struct key_data key_kp_0[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Op"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2p"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3p"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4p"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5p"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6p"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7p"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8p"},
};

static const struct key_data key_kp_1[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Oq"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2q"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3q"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4q"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5q"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6q"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7q"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8q"},
};

static const struct key_data key_kp_2[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Or"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2r"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3r"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4r"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5r"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6r"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7r"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8r"},
};

static const struct key_data key_kp_3[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Os"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2s"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3s"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4s"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5s"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6s"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7s"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8s"},
};

static const struct key_data key_kp_4[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Ot"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2t"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3t"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4t"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5t"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6t"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7t"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8t"},
};

static const struct key_data key_kp_5[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Ou"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2u"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3u"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4u"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5u"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6u"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7u"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8u"},
};

static const struct key_data key_kp_6[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Ov"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2v"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3v"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4v"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5v"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6v"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7v"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8v"},
};

static const struct key_data key_kp_7[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Ow"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2w"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3w"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4w"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5w"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6w"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7w"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8w"},
};

static const struct key_data key_kp_8[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Ox"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2x"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3x"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4x"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5x"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6x"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7x"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8x"},
};

static const struct key_data key_kp_9[] = {
    {MOD_NONE,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033Oy"},
    {MOD_SHIFT,                      CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O2y"},
    {MOD_ALT,                        CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O3y"},
    {MOD_SHIFT | MOD_ALT,            CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O4y"},
    {MOD_CTRL,                       CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O5y"},
    {MOD_SHIFT | MOD_CTRL,           CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O6y"},
    {MOD_ALT | MOD_CTRL,             CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O7y"},
    {MOD_SHIFT | MOD_ALT | MOD_CTRL, CURSOR_KEYS_DONTCARE, KEYPAD_APPLICATION, "\033O8y"},
};

#define ALEN(a) (sizeof(a) / sizeof(a[0]))
static const struct key_map key_map[] = {
    {XKB_KEY_Escape,    ALEN(key_escape),    key_escape},
    {XKB_KEY_Return,    ALEN(key_return),    key_return},
    {XKB_KEY_Tab,       ALEN(key_tab),       key_tab},
    {XKB_KEY_ISO_Left_Tab, ALEN(key_backtab),   key_backtab},
    {XKB_KEY_BackSpace, ALEN(key_backspace), key_backspace},
    {XKB_KEY_Up,        ALEN(key_up),        key_up},
    {XKB_KEY_Down,      ALEN(key_down),      key_down},
    {XKB_KEY_Right,     ALEN(key_right),     key_right},
    {XKB_KEY_Left,      ALEN(key_left),      key_left},
    {XKB_KEY_Home,      ALEN(key_home),      key_home},
    {XKB_KEY_End,       ALEN(key_end),       key_end},
    {XKB_KEY_Insert,    ALEN(key_insert),    key_insert},
    {XKB_KEY_Delete,    ALEN(key_delete),    key_delete},
    {XKB_KEY_Page_Up,   ALEN(key_pageup),    key_pageup},
    {XKB_KEY_Page_Down, ALEN(key_pagedown),  key_pagedown},
    {XKB_KEY_F1,        ALEN(key_f1),        key_f1},
    {XKB_KEY_F2,        ALEN(key_f2),        key_f2},
    {XKB_KEY_F3,        ALEN(key_f3),        key_f3},
    {XKB_KEY_F4,        ALEN(key_f4),        key_f4},
    {XKB_KEY_F5,        ALEN(key_f5),        key_f5},
    {XKB_KEY_F6,        ALEN(key_f6),        key_f6},
    {XKB_KEY_F7,        ALEN(key_f7),        key_f7},
    {XKB_KEY_F8,        ALEN(key_f8),        key_f8},
    {XKB_KEY_F9,        ALEN(key_f9),        key_f9},
    {XKB_KEY_F10,       ALEN(key_f10),       key_f10},
    {XKB_KEY_F11,       ALEN(key_f11),       key_f11},
    {XKB_KEY_F12,       ALEN(key_f12),       key_f12},
    {XKB_KEY_F13,       ALEN(key_f13),       key_f13},
    {XKB_KEY_F14,       ALEN(key_f14),       key_f14},
    {XKB_KEY_F15,       ALEN(key_f15),       key_f15},
    {XKB_KEY_F16,       ALEN(key_f16),       key_f16},
    {XKB_KEY_F17,       ALEN(key_f17),       key_f17},
    {XKB_KEY_F18,       ALEN(key_f18),       key_f18},
    {XKB_KEY_F19,       ALEN(key_f19),       key_f19},
    {XKB_KEY_F20,       ALEN(key_f20),       key_f20},
    {XKB_KEY_F21,       ALEN(key_f21),       key_f21},
    {XKB_KEY_F22,       ALEN(key_f22),       key_f22},
    {XKB_KEY_F23,       ALEN(key_f23),       key_f23},
    {XKB_KEY_F24,       ALEN(key_f24),       key_f24},
    {XKB_KEY_F25,       ALEN(key_f25),       key_f25},
    {XKB_KEY_F26,       ALEN(key_f26),       key_f26},
    {XKB_KEY_F27,       ALEN(key_f27),       key_f27},
    {XKB_KEY_F28,       ALEN(key_f28),       key_f28},
    {XKB_KEY_F29,       ALEN(key_f29),       key_f29},
    {XKB_KEY_F30,       ALEN(key_f30),       key_f30},
    {XKB_KEY_F31,       ALEN(key_f31),       key_f31},
    {XKB_KEY_F32,       ALEN(key_f32),       key_f32},
    {XKB_KEY_F33,       ALEN(key_f33),       key_f33},
    {XKB_KEY_F34,       ALEN(key_f34),       key_f34},
    {XKB_KEY_F35,       ALEN(key_f35),       key_f35},
    {XKB_KEY_KP_Up,     ALEN(key_kp_up),     key_kp_up},
    {XKB_KEY_KP_Down,   ALEN(key_kp_down),   key_kp_down},
    {XKB_KEY_KP_Right,  ALEN(key_kp_right),  key_kp_right},
    {XKB_KEY_KP_Left,   ALEN(key_kp_left),   key_kp_left},
    {XKB_KEY_KP_Home,   ALEN(key_kp_home),   key_kp_home},
    {XKB_KEY_KP_End,    ALEN(key_kp_end),    key_kp_end},
    {XKB_KEY_KP_Insert, ALEN(key_kp_insert), key_kp_insert},
    {XKB_KEY_KP_Delete, ALEN(key_kp_delete), key_kp_delete},
    {XKB_KEY_KP_Page_Up,ALEN(key_kp_pageup), key_kp_pageup},
    {XKB_KEY_KP_Page_Down, ALEN(key_kp_pagedown), key_kp_pagedown},
    {XKB_KEY_KP_Divide, ALEN(key_kp_divide), key_kp_divide},
    {XKB_KEY_KP_Multiply,ALEN(key_kp_multiply), key_kp_multiply},
    {XKB_KEY_KP_Subtract,ALEN(key_kp_subtract), key_kp_subtract},
    {XKB_KEY_KP_Add,    ALEN(key_kp_add), key_kp_add},
    {XKB_KEY_KP_Separator,ALEN(key_kp_separator), key_kp_separator},
    {XKB_KEY_KP_0,      ALEN(key_kp_0), key_kp_0},
    {XKB_KEY_KP_1,      ALEN(key_kp_1), key_kp_1},
    {XKB_KEY_KP_2,      ALEN(key_kp_2), key_kp_2},
    {XKB_KEY_KP_3,      ALEN(key_kp_3), key_kp_3},
    {XKB_KEY_KP_4,      ALEN(key_kp_4), key_kp_4},
    {XKB_KEY_KP_5,      ALEN(key_kp_5), key_kp_5},
    {XKB_KEY_KP_6,      ALEN(key_kp_6), key_kp_6},
    {XKB_KEY_KP_7,      ALEN(key_kp_7), key_kp_7},
    {XKB_KEY_KP_8,      ALEN(key_kp_8), key_kp_8},
    {XKB_KEY_KP_9,      ALEN(key_kp_9), key_kp_9},
};
#undef ALEN
