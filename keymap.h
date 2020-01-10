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

static const struct key_data key_kp_divide[] = {DEFAULT_MODS_FOR_KP(o)};
static const struct key_data key_kp_multiply[] = {DEFAULT_MODS_FOR_KP(j)};
static const struct key_data key_kp_subtract[] = {DEFAULT_MODS_FOR_KP(m)};
static const struct key_data key_kp_add[] = {DEFAULT_MODS_FOR_KP(k)};
static const struct key_data key_kp_separator[] = {DEFAULT_MODS_FOR_KP(l)};
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
    {XKB_KEY_KP_Begin,  ALEN(key_kp_begin),  key_kp_begin},
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
