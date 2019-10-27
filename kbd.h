#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <xkbcommon/xkbcommon.h>

struct kbd {
    struct xkb_context *xkb;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    struct xkb_compose_table *xkb_compose_table;
    struct xkb_compose_state *xkb_compose_state;
    struct {
        int fd;

        bool dont_re_repeat;
        int32_t delay;
        int32_t rate;
        uint32_t key;
    } repeat;

    xkb_mod_index_t mod_shift;
    xkb_mod_index_t mod_alt;
    xkb_mod_index_t mod_ctrl;
    xkb_mod_index_t mod_meta;

    /* Enabled modifiers */
    bool shift;
    bool alt;
    bool ctrl;
    bool meta;
};

void kbd_destroy(struct kbd *kbd);
