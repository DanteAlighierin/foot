#include "input.h"

#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <threads.h>
#include <locale.h>
#include <sys/mman.h>

#include <linux/input-event-codes.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "input"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "terminal.h"
#include "render.h"
#include "keymap.h"
#include "commands.h"
#include "selection.h"
#include "vt.h"

static void
keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                uint32_t format, int32_t fd, uint32_t size)
{
    struct terminal *term = data;

    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

    /* TODO: free old context + keymap */

    term->kbd.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    term->kbd.xkb_keymap = xkb_keymap_new_from_string(
        term->kbd.xkb, map_str, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    /* TODO: initialize in enter? */
    term->kbd.xkb_state = xkb_state_new(term->kbd.xkb_keymap);

    term->kbd.mod_shift = xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Shift");
    term->kbd.mod_alt = xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Mod1") ;
    term->kbd.mod_ctrl = xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Control");

    /* Compose (dead keys) */
    term->kbd.xkb_compose_table = xkb_compose_table_new_from_locale(
        term->kbd.xkb, setlocale(LC_CTYPE, NULL), XKB_COMPOSE_COMPILE_NO_FLAGS);
    term->kbd.xkb_compose_state = xkb_compose_state_new(
        term->kbd.xkb_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);

    munmap(map_str, size);
    close(fd);
}

static void
keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
               struct wl_surface *surface, struct wl_array *keys)
{
    LOG_DBG("enter");
#if 0
    uint32_t *key;
    wl_array_for_each(key, keys)
        xkb_state_update_key(xkb_state, *key, 1);
#endif
    term_focus_in((struct terminal *)data);
}

static void
keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
               struct wl_surface *surface)
{
    struct terminal *term = data;
    term_focus_out(term);

    mtx_lock(&term->kbd.repeat.mutex);
    if (term->kbd.repeat.cmd != REPEAT_EXIT) {
        term->kbd.repeat.cmd = REPEAT_STOP;
        cnd_signal(&term->kbd.repeat.cond);
    }
    mtx_unlock(&term->kbd.repeat.mutex);
}

static void
keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
             uint32_t time, uint32_t key, uint32_t state)
{
    struct terminal *term = data;

    const xkb_mod_mask_t ctrl = 1 << term->kbd.mod_ctrl;
    const xkb_mod_mask_t alt = 1 << term->kbd.mod_alt;
    const xkb_mod_mask_t shift = 1 << term->kbd.mod_shift;

    if (state == XKB_KEY_UP) {
        mtx_lock(&term->kbd.repeat.mutex);
        if (term->kbd.repeat.key == key) {
            if (term->kbd.repeat.cmd != REPEAT_EXIT) {
                term->kbd.repeat.cmd = REPEAT_STOP;
                cnd_signal(&term->kbd.repeat.cond);
            }
        }
        mtx_unlock(&term->kbd.repeat.mutex);
        return;
    }

    key += 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(term->kbd.xkb_state, key);

#if 0
    char foo[100];
    xkb_keysym_get_name(sym, foo, sizeof(foo));
    LOG_ERR("%s", foo);
#endif

    xkb_compose_state_feed(term->kbd.xkb_compose_state, sym);
    enum xkb_compose_status compose_status = xkb_compose_state_get_status(
        term->kbd.xkb_compose_state);

    if (compose_status == XKB_COMPOSE_COMPOSING)
        return;

    xkb_mod_mask_t mods = xkb_state_serialize_mods(
        term->kbd.xkb_state, XKB_STATE_MODS_DEPRESSED);
    //xkb_mod_mask_t consumed = xkb_state_key_get_consumed_mods(term->kbd.xkb_state, key);
    xkb_mod_mask_t consumed = 0x0;
    xkb_mod_mask_t significant = ctrl | alt | shift;
    xkb_mod_mask_t effective_mods = mods & ~consumed & significant;

#if 0
    for (size_t i = 0; i < 32; i++) {
        if (mods & (1 << i)) {
            LOG_DBG("%s", xkb_keymap_mod_get_name(term->kbd.xkb_keymap, i));
        }
    }
#endif

    LOG_DBG("sym=%u, mod=0x%08x, consumed=0x%08x, significant=0x%08x, "
            "effective=0x%08x",
            sym, mods, consumed, significant, effective_mods);

    bool found_map = false;

    enum modifier keymap_mods = MOD_NONE;
    keymap_mods |= term->kbd.shift ? MOD_SHIFT : MOD_NONE;
    keymap_mods |= term->kbd.alt ? MOD_ALT : MOD_NONE;
    keymap_mods |= term->kbd.ctrl ? MOD_CTRL : MOD_NONE;

    if (effective_mods == shift) {
        if (sym == XKB_KEY_Page_Up) {
            cmd_scrollback_up(term, term->rows);
            found_map = true;
        }

        else if (sym == XKB_KEY_Page_Down) {
            cmd_scrollback_down(term, term->rows);
            found_map = true;
        }
    }

    else if (effective_mods == (shift | ctrl)) {
        if (sym == XKB_KEY_C) {
            selection_to_clipboard(term, serial);
            found_map = true;
        }

        else if (sym == XKB_KEY_V) {
            selection_from_clipboard(term, serial);
            found_map = true;
        }
    }

    for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]) && !found_map; i++) {
        const struct key_map *k = &key_map[i];
        if (k->sym != sym)
            continue;

        for (size_t j = 0; j < k->count; j++) {
            const struct key_data *info = &k->data[j];
            if (info->modifiers != MOD_ANY && info->modifiers != keymap_mods)
                continue;

            if (info->cursor_keys_mode != CURSOR_KEYS_DONTCARE &&
                info->cursor_keys_mode != term->cursor_keys_mode)
                continue;

            if (info->keypad_keys_mode != KEYPAD_DONTCARE &&
                info->keypad_keys_mode != term->keypad_keys_mode)
                continue;

            vt_to_slave(term, info->seq, strlen(info->seq));
            found_map = true;

            if (term->grid->view != term->grid->offset) {
                term->grid->view = term->grid->offset;
                term_damage_all(term);
            }

            selection_cancel(term);
            break;
        }
    }

    if (!found_map) {
        char buf[64] = {0};
        int count = 0;

        if (compose_status == XKB_COMPOSE_COMPOSED) {
            count = xkb_compose_state_get_utf8(
                term->kbd.xkb_compose_state, buf, sizeof(buf));
            xkb_compose_state_reset(term->kbd.xkb_compose_state);
        } else {
            count = xkb_state_key_get_utf8(
                term->kbd.xkb_state, key, buf, sizeof(buf));
        }

        if (count > 0) {
            if (effective_mods & alt)
                vt_to_slave(term, "\x1b", 1);

            vt_to_slave(term, buf, count);

            if (term->grid->view != term->grid->offset) {
                term->grid->view = term->grid->offset;
                term_damage_all(term);
            }

            selection_cancel(term);
        }
    }

    mtx_lock(&term->kbd.repeat.mutex);
    if (!term->kbd.repeat.dont_re_repeat) {
        if (term->kbd.repeat.cmd != REPEAT_EXIT) {
            term->kbd.repeat.cmd = REPEAT_START;
            term->kbd.repeat.key = key - 8;
            cnd_signal(&term->kbd.repeat.cond);
        }
    }
    mtx_unlock(&term->kbd.repeat.mutex);
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
                   uint32_t mods_depressed, uint32_t mods_latched,
                   uint32_t mods_locked, uint32_t group)
{
    struct terminal *term = data;

    LOG_DBG("modifiers: depressed=0x%x, latched=0x%x, locked=0x%x, group=%u",
            mods_depressed, mods_latched, mods_locked, group);

    xkb_state_update_mask(
        term->kbd.xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);

    /* Update state of modifiers we're interrested in for e.g mouse events */
    term->kbd.shift = xkb_state_mod_index_is_active(
        term->kbd.xkb_state, term->kbd.mod_shift, XKB_STATE_MODS_DEPRESSED);
    term->kbd.alt = xkb_state_mod_index_is_active(
        term->kbd.xkb_state, term->kbd.mod_alt, XKB_STATE_MODS_DEPRESSED);
    term->kbd.ctrl = xkb_state_mod_index_is_active(
        term->kbd.xkb_state, term->kbd.mod_ctrl, XKB_STATE_MODS_DEPRESSED);
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                     int32_t rate, int32_t delay)
{
    struct terminal *term = data;
    LOG_DBG("keyboard repeat: rate=%d, delay=%d", rate, delay);
    term->kbd.repeat.rate = rate;
    term->kbd.repeat.delay = delay;
}

const struct wl_keyboard_listener keyboard_listener = {
    .keymap = &keyboard_keymap,
    .enter = &keyboard_enter,
    .leave = &keyboard_leave,
    .key = &keyboard_key,
    .modifiers = &keyboard_modifiers,
    .repeat_info = &keyboard_repeat_info,
};

void
input_repeat(struct terminal *term, uint32_t key)
{
    keyboard_key(term, NULL, 0, 0, key, XKB_KEY_DOWN);
}

static void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface,
                 wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct terminal *term = data;

    int x = wl_fixed_to_int(surface_x) * 1; //scale
    int y = wl_fixed_to_int(surface_y) * 1; //scale

    term->mouse.col = x / term->cell_width;
    term->mouse.row = y / term->cell_height;

    render_update_cursor_surface(term);
}

static void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface)
{
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
                  uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct terminal *term = data;

    int x = wl_fixed_to_int(surface_x) * 1;//backend->monitor->scale;
    int y = wl_fixed_to_int(surface_y) * 1;//backend->monitor->scale;

    int col = x / term->cell_width;
    int row = y / term->cell_height;

    if (col < 0 || row < 0 || col >= term->cols || row >= term->rows)
        return;

    bool update_selection = term->mouse.button == BTN_LEFT;
    bool update_selection_early = term->selection.end.row == -1;

    if (update_selection && update_selection_early)
        selection_update(term, col, row);

    if (col == term->mouse.col && row == term->mouse.row)
        return;

    term->mouse.col = col;
    term->mouse.row = row;

    if (update_selection && !update_selection_early)
        selection_update(term, col, row);

    term_mouse_motion(
        term, term->mouse.button, term->mouse.row, term->mouse.col,
        term->kbd.shift, term->kbd.alt, term->kbd.ctrl);
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
                  uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    LOG_DBG("BUTTON: button=%x, state=%u", button, state);

    struct terminal *term = data;

    switch (state) {
    case WL_POINTER_BUTTON_STATE_PRESSED:
        if (button == BTN_LEFT)
            selection_start(term, term->mouse.col, term->mouse.row);
        else {
            if (button == BTN_MIDDLE)
                selection_from_primary(term);
            selection_cancel(term);
        }

        term->mouse.button = button; /* For motion events */
        term_mouse_down(term, button, term->mouse.row, term->mouse.col,
                        term->kbd.shift, term->kbd.alt, term->kbd.ctrl);
        break;

    case WL_POINTER_BUTTON_STATE_RELEASED:
        if (button != BTN_LEFT || term->selection.end.col == -1)
            selection_cancel(term);
        else
            selection_finalize(term, serial);

        term->mouse.button = 0; /* For motion events */
        term_mouse_up(term, button, term->mouse.row, term->mouse.col,
                      term->kbd.shift, term->kbd.alt, term->kbd.ctrl);
        break;
    }
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
                uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct terminal *term = data;

    /* TODO: generate button event for BTN_FORWARD/BTN_BACK? */

    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    int amount = wl_fixed_to_int(value);

    if (amount < 0) {
        for (int i = 0; i < -amount; i++) {
            term_mouse_down(term, BTN_BACK, term->mouse.row, term->mouse.col,
                            term->kbd.shift, term->kbd.alt, term->kbd.ctrl);
        }
        cmd_scrollback_up(term, -amount);
    } else {
        for (int i = 0; i < amount; i++) {
            term_mouse_down(term, BTN_FORWARD, term->mouse.row, term->mouse.col,
                            term->kbd.shift, term->kbd.alt, term->kbd.ctrl);
        }
        cmd_scrollback_down(term, amount);
    }
}

static void
wl_pointer_frame(void *data, struct wl_pointer *wl_pointer)
{
}

static void
wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
                       uint32_t axis_source)
{
}

static void
wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
                     uint32_t time, uint32_t axis)
{
}

static void
wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                         uint32_t axis, int32_t discrete)
{
}

const struct wl_pointer_listener pointer_listener = {
    .enter = wl_pointer_enter,
    .leave = wl_pointer_leave,
    .motion = wl_pointer_motion,
    .button = wl_pointer_button,
    .axis = wl_pointer_axis,
    .frame = wl_pointer_frame,
    .axis_source = wl_pointer_axis_source,
    .axis_stop = wl_pointer_axis_stop,
    .axis_discrete = wl_pointer_axis_discrete,
};
