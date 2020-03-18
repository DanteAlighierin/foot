#include "input.h"

#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <threads.h>
#include <locale.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>

#include <linux/input-event-codes.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <xdg-shell.h>

#define LOG_MODULE "input"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "commands.h"
#include "keymap.h"
#include "quirks.h"
#include "render.h"
#include "search.h"
#include "selection.h"
#include "terminal.h"
#include "vt.h"

#define ALEN(v) (sizeof(v) / sizeof(v[0]))

void
input_execute_binding(struct terminal *term, enum binding_action action,
                      uint32_t serial)
{
    switch (action) {
    case BIND_ACTION_NONE:
        break;

    case BIND_ACTION_SCROLLBACK_UP:
        cmd_scrollback_up(term, term->rows);
        break;

    case BIND_ACTION_SCROLLBACK_DOWN:
        cmd_scrollback_down(term, term->rows);
        break;

    case BIND_ACTION_CLIPBOARD_COPY:
        selection_to_clipboard(term, serial);
        break;

    case BIND_ACTION_CLIPBOARD_PASTE:
        selection_from_clipboard(term, serial);
        term_reset_view(term);
        break;

    case BIND_ACTION_PRIMARY_PASTE:
        selection_from_primary(term);
        break;

    case BIND_ACTION_SEARCH_START:
        search_begin(term);
        break;

    case BIND_ACTION_FONT_SIZE_UP:
        term_font_size_increase(term);
        break;

    case BIND_ACTION_FONT_SIZE_DOWN:
        term_font_size_decrease(term);
        break;

    case BIND_ACTION_FONT_SIZE_RESET:
        term_font_size_reset(term);
        break;

    case BIND_ACTION_SPAWN_TERMINAL:
        term_spawn_new(term);
        break;

    case BIND_ACTION_MINIMIZE:
        xdg_toplevel_set_minimized(term->window->xdg_toplevel);
        break;

    case BIND_ACTION_MAXIMIZE:
        if (term->window->is_fullscreen)
            xdg_toplevel_unset_fullscreen(term->window->xdg_toplevel);
        if (term->window->is_maximized)
            xdg_toplevel_unset_maximized(term->window->xdg_toplevel);
        else
            xdg_toplevel_set_maximized(term->window->xdg_toplevel);
        break;

    case BIND_ACTION_FULLSCREEN:
        if (term->window->is_fullscreen)
            xdg_toplevel_unset_fullscreen(term->window->xdg_toplevel);
        else
            xdg_toplevel_set_fullscreen(term->window->xdg_toplevel, NULL);
        break;

    case BIND_ACTION_COUNT:
        assert(false);
        break;
    }
}

bool
input_parse_key_binding_for_action(
    struct xkb_keymap *keymap, enum binding_action action,
    const char *combos, key_binding_list_t *bindings)
{
    if (combos == NULL)
        return true;

    xkb_mod_mask_t mod_mask = 0;
    xkb_keysym_t sym = XKB_KEY_NoSymbol;

    char *copy = strdup(combos);

    for (char *save1 = NULL, *combo = strtok_r(copy, " ", &save1);
         combo != NULL;
         combo = strtok_r(NULL, " ", &save1))
    {
        LOG_DBG("%s", combo);
        for (char *save2 = NULL, *key = strtok_r(combo, "+", &save2),
                 *next_key = strtok_r(NULL, "+", &save2);
             key != NULL;
             key = next_key, next_key = strtok_r(NULL, "+", &save2))
        {
            if (next_key != NULL) {
                /* Modifier */
                xkb_mod_index_t mod = xkb_keymap_mod_get_index(keymap, key);

                if (mod == -1) {
                    LOG_ERR("%s: not a valid modifier name", key);
                    free(copy);
                    return false;
                }

                mod_mask |= 1 << mod;
                LOG_DBG("MOD: %d - %s", mod, key);
            } else {
                /* Symbol */
                sym = xkb_keysym_from_name(key, 0);

                if (sym == XKB_KEY_NoSymbol) {
                    LOG_ERR("%s: key binding is not a valid XKB symbol name",
                            key);
                    break;
                }
            }
        }

        LOG_DBG("action=%u: mods=0x%08x, sym=%d", action, mod_mask, sym);

        if (sym == XKB_KEY_NoSymbol)
            continue;

        assert(sym != 0);
        if (bindings != NULL) {
            const struct key_binding binding = {
                .mods = mod_mask,
                .sym = sym,
                .action = action,
            };

            tll_push_back(*bindings, binding);
        }
    }

    free(copy);
    return true;
}

static void
keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                uint32_t format, int32_t fd, uint32_t size)
{
    LOG_DBG("keyboard_keymap: keyboard=%p (format=%u, size=%u)",
            wl_keyboard, format, size);

    struct wayland *wayl = data;

    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

    if (wayl->kbd.xkb_compose_state != NULL) {
        xkb_compose_state_unref(wayl->kbd.xkb_compose_state);
        wayl->kbd.xkb_compose_state = NULL;
    }
    if (wayl->kbd.xkb_compose_table != NULL) {
        xkb_compose_table_unref(wayl->kbd.xkb_compose_table);
        wayl->kbd.xkb_compose_table = NULL;
    }
    if (wayl->kbd.xkb_keymap != NULL) {
        xkb_keymap_unref(wayl->kbd.xkb_keymap);
        wayl->kbd.xkb_keymap = NULL;
    }
    if (wayl->kbd.xkb_state != NULL) {
        xkb_state_unref(wayl->kbd.xkb_state);
        wayl->kbd.xkb_state = NULL;
    }
    if (wayl->kbd.xkb != NULL) {
        xkb_context_unref(wayl->kbd.xkb);
        wayl->kbd.xkb = NULL;
    }
    tll_free(wayl->kbd.bindings.key);

    wayl->kbd.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    wayl->kbd.xkb_keymap = xkb_keymap_new_from_string(
        wayl->kbd.xkb, map_str, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    /* TODO: initialize in enter? */
    wayl->kbd.xkb_state = xkb_state_new(wayl->kbd.xkb_keymap);

    wayl->kbd.mod_shift = xkb_keymap_mod_get_index(wayl->kbd.xkb_keymap, "Shift");
    wayl->kbd.mod_alt = xkb_keymap_mod_get_index(wayl->kbd.xkb_keymap, "Mod1") ;
    wayl->kbd.mod_ctrl = xkb_keymap_mod_get_index(wayl->kbd.xkb_keymap, "Control");
    wayl->kbd.mod_meta = xkb_keymap_mod_get_index(wayl->kbd.xkb_keymap, "Mod4");

    /* Compose (dead keys) */
    wayl->kbd.xkb_compose_table = xkb_compose_table_new_from_locale(
        wayl->kbd.xkb, setlocale(LC_CTYPE, NULL), XKB_COMPOSE_COMPILE_NO_FLAGS);
    wayl->kbd.xkb_compose_state = xkb_compose_state_new(
        wayl->kbd.xkb_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);

    munmap(map_str, size);
    close(fd);

    for (size_t i = 0; i < BIND_ACTION_COUNT; i++) {
        input_parse_key_binding_for_action(
            wayl->kbd.xkb_keymap, i,
            wayl->conf->bindings.key[i], &wayl->kbd.bindings.key);

        input_parse_key_binding_for_action(
            wayl->kbd.xkb_keymap, i,
            wayl->conf->bindings.search[i], &wayl->kbd.bindings.search);
    }
}

static void
keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
               struct wl_surface *surface, struct wl_array *keys)
{
    assert(surface != NULL);

    struct wayland *wayl = data;
    struct wl_window *win = wl_surface_get_user_data(surface);
    struct terminal *term = win->term;

    LOG_DBG("keyboard_enter: keyboard=%p, serial=%u, surface=%p",
            wl_keyboard, serial, surface);

    wayl->kbd_focus = term;
    wayl->input_serial = serial;

    term_kbd_focus_in(wayl->kbd_focus);
}

static bool
start_repeater(struct wayland *wayl, uint32_t key)
{
    if (wayl->kbd.repeat.dont_re_repeat)
        return true;

    struct itimerspec t = {
        .it_value = {.tv_sec = 0, .tv_nsec = wayl->kbd.repeat.delay * 1000000},
        .it_interval = {.tv_sec = 0, .tv_nsec = 1000000000 / wayl->kbd.repeat.rate},
    };

    if (t.it_value.tv_nsec >= 1000000000) {
        t.it_value.tv_sec += t.it_value.tv_nsec / 1000000000;
        t.it_value.tv_nsec %= 1000000000;
    }
    if (t.it_interval.tv_nsec >= 1000000000) {
        t.it_interval.tv_sec += t.it_interval.tv_nsec / 1000000000;
        t.it_interval.tv_nsec %= 1000000000;
    }
    if (timerfd_settime(wayl->kbd.repeat.fd, 0, &t, NULL) < 0) {
        LOG_ERRNO("failed to arm keyboard repeat timer");
        return false;
    }

    wayl->kbd.repeat.key = key;
    return true;
}

static bool
stop_repeater(struct wayland *wayl, uint32_t key)
{
    if (key != -1 && key != wayl->kbd.repeat.key)
        return true;

    if (timerfd_settime(wayl->kbd.repeat.fd, 0, &(struct itimerspec){{0}}, NULL) < 0) {
        LOG_ERRNO("failed to disarm keyboard repeat timer");
        return false;
    }

    return true;
}

static void
keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
               struct wl_surface *surface)
{
    struct wayland *wayl = data;

    LOG_DBG("keyboard_leave: keyboard=%p, serial=%u, surface=%p",
            wl_keyboard, serial, surface);

    assert(
        wayl->kbd_focus == NULL ||
        surface == NULL ||  /* Seen on Sway 1.2 */
        ((const struct wl_window *)wl_surface_get_user_data(surface))->term == wayl->kbd_focus
        );

    struct terminal *old_focused = wayl->kbd_focus;
    wayl->kbd_focus = NULL;

    stop_repeater(wayl, -1);
    wayl->kbd.shift = false;;
    wayl->kbd.alt = false;;
    wayl->kbd.ctrl = false;;
    wayl->kbd.meta = false;;
    xkb_compose_state_reset(wayl->kbd.xkb_compose_state);

    if (old_focused != NULL)
        term_kbd_focus_out(old_focused);
    else {
        /*
         * Sway bug - under certain conditions we get a
         * keyboard_leave() (and keyboard_key()) without first having
         * received a keyboard_enter()
         */
        LOG_WARN(
            "compositor sent keyboard_leave event without a keyboard_enter "
            "event: surface=%p", surface);
    }
}

static const struct key_data *
keymap_data_for_sym(xkb_keysym_t sym, size_t *count)
{
    switch (sym) {
    case XKB_KEY_Escape:       *count = ALEN(key_escape);       return key_escape;
    case XKB_KEY_Return:       *count = ALEN(key_return);       return key_return;
    case XKB_KEY_Tab:          *count = ALEN(key_tab);          return key_tab;
    case XKB_KEY_ISO_Left_Tab: *count = ALEN(key_backtab);      return key_backtab;
    case XKB_KEY_BackSpace:    *count = ALEN(key_backspace);    return key_backspace;
    case XKB_KEY_Up:           *count = ALEN(key_up);           return key_up;
    case XKB_KEY_Down:         *count = ALEN(key_down);         return key_down;
    case XKB_KEY_Right:        *count = ALEN(key_right);        return key_right;
    case XKB_KEY_Left:         *count = ALEN(key_left);         return key_left;
    case XKB_KEY_Home:         *count = ALEN(key_home);         return key_home;
    case XKB_KEY_End:          *count = ALEN(key_end);          return key_end;
    case XKB_KEY_Insert:       *count = ALEN(key_insert);       return key_insert;
    case XKB_KEY_Delete:       *count = ALEN(key_delete);       return key_delete;
    case XKB_KEY_Page_Up:      *count = ALEN(key_pageup);       return key_pageup;
    case XKB_KEY_Page_Down:    *count = ALEN(key_pagedown);     return key_pagedown;
    case XKB_KEY_F1:           *count = ALEN(key_f1);           return key_f1;
    case XKB_KEY_F2:           *count = ALEN(key_f2);           return key_f2;
    case XKB_KEY_F3:           *count = ALEN(key_f3);           return key_f3;
    case XKB_KEY_F4:           *count = ALEN(key_f4);           return key_f4;
    case XKB_KEY_F5:           *count = ALEN(key_f5);           return key_f5;
    case XKB_KEY_F6:           *count = ALEN(key_f6);           return key_f6;
    case XKB_KEY_F7:           *count = ALEN(key_f7);           return key_f7;
    case XKB_KEY_F8:           *count = ALEN(key_f8);           return key_f8;
    case XKB_KEY_F9:           *count = ALEN(key_f9);           return key_f9;
    case XKB_KEY_F10:          *count = ALEN(key_f10);          return key_f10;
    case XKB_KEY_F11:          *count = ALEN(key_f11);          return key_f11;
    case XKB_KEY_F12:          *count = ALEN(key_f12);          return key_f12;
    case XKB_KEY_F13:          *count = ALEN(key_f13);          return key_f13;
    case XKB_KEY_F14:          *count = ALEN(key_f14);          return key_f14;
    case XKB_KEY_F15:          *count = ALEN(key_f15);          return key_f15;
    case XKB_KEY_F16:          *count = ALEN(key_f16);          return key_f16;
    case XKB_KEY_F17:          *count = ALEN(key_f17);          return key_f17;
    case XKB_KEY_F18:          *count = ALEN(key_f18);          return key_f18;
    case XKB_KEY_F19:          *count = ALEN(key_f19);          return key_f19;
    case XKB_KEY_F20:          *count = ALEN(key_f20);          return key_f20;
    case XKB_KEY_F21:          *count = ALEN(key_f21);          return key_f21;
    case XKB_KEY_F22:          *count = ALEN(key_f22);          return key_f22;
    case XKB_KEY_F23:          *count = ALEN(key_f23);          return key_f23;
    case XKB_KEY_F24:          *count = ALEN(key_f24);          return key_f24;
    case XKB_KEY_F25:          *count = ALEN(key_f25);          return key_f25;
    case XKB_KEY_F26:          *count = ALEN(key_f26);          return key_f26;
    case XKB_KEY_F27:          *count = ALEN(key_f27);          return key_f27;
    case XKB_KEY_F28:          *count = ALEN(key_f28);          return key_f28;
    case XKB_KEY_F29:          *count = ALEN(key_f29);          return key_f29;
    case XKB_KEY_F30:          *count = ALEN(key_f30);          return key_f30;
    case XKB_KEY_F31:          *count = ALEN(key_f31);          return key_f31;
    case XKB_KEY_F32:          *count = ALEN(key_f32);          return key_f32;
    case XKB_KEY_F33:          *count = ALEN(key_f33);          return key_f33;
    case XKB_KEY_F34:          *count = ALEN(key_f34);          return key_f34;
    case XKB_KEY_F35:          *count = ALEN(key_f35);          return key_f35;
    case XKB_KEY_KP_Up:        *count = ALEN(key_kp_up);        return key_kp_up;
    case XKB_KEY_KP_Down:      *count = ALEN(key_kp_down);      return key_kp_down;
    case XKB_KEY_KP_Right:     *count = ALEN(key_kp_right);     return key_kp_right;
    case XKB_KEY_KP_Left:      *count = ALEN(key_kp_left);      return key_kp_left;
    case XKB_KEY_KP_Begin:     *count = ALEN(key_kp_begin);     return key_kp_begin;
    case XKB_KEY_KP_Home:      *count = ALEN(key_kp_home);      return key_kp_home;
    case XKB_KEY_KP_End:       *count = ALEN(key_kp_end);       return key_kp_end;
    case XKB_KEY_KP_Insert:    *count = ALEN(key_kp_insert);    return key_kp_insert;
    case XKB_KEY_KP_Delete:    *count = ALEN(key_kp_delete);    return key_kp_delete;
    case XKB_KEY_KP_Page_Up:   *count = ALEN(key_kp_pageup);    return key_kp_pageup;
    case XKB_KEY_KP_Page_Down: *count = ALEN(key_kp_pagedown);  return key_kp_pagedown;
    case XKB_KEY_KP_Enter:     *count = ALEN(key_kp_enter);     return key_kp_enter;
    case XKB_KEY_KP_Divide:    *count = ALEN(key_kp_divide);    return key_kp_divide;
    case XKB_KEY_KP_Multiply:  *count = ALEN(key_kp_multiply);  return key_kp_multiply;
    case XKB_KEY_KP_Subtract:  *count = ALEN(key_kp_subtract);  return key_kp_subtract;
    case XKB_KEY_KP_Add:       *count = ALEN(key_kp_add);       return key_kp_add;
    case XKB_KEY_KP_Separator: *count = ALEN(key_kp_separator); return key_kp_separator;
    case XKB_KEY_KP_0:         *count = ALEN(key_kp_0);         return key_kp_0;
    case XKB_KEY_KP_1:         *count = ALEN(key_kp_1);         return key_kp_1;
    case XKB_KEY_KP_2:         *count = ALEN(key_kp_2);         return key_kp_2;
    case XKB_KEY_KP_3:         *count = ALEN(key_kp_3);         return key_kp_3;
    case XKB_KEY_KP_4:         *count = ALEN(key_kp_4);         return key_kp_4;
    case XKB_KEY_KP_5:         *count = ALEN(key_kp_5);         return key_kp_5;
    case XKB_KEY_KP_6:         *count = ALEN(key_kp_6);         return key_kp_6;
    case XKB_KEY_KP_7:         *count = ALEN(key_kp_7);         return key_kp_7;
    case XKB_KEY_KP_8:         *count = ALEN(key_kp_8);         return key_kp_8;
    case XKB_KEY_KP_9:         *count = ALEN(key_kp_9);         return key_kp_9;
    }

    return NULL;
}

static const struct key_data *
keymap_lookup(struct terminal *term, xkb_keysym_t sym, enum modifier mods)
{
    size_t count;
    const struct key_data *info = keymap_data_for_sym(sym, &count);

    if (info == NULL)
        return NULL;

    for (size_t j = 0; j < count; j++) {
        if (info[j].modifiers != MOD_ANY && info[j].modifiers != mods)
            continue;

        if (info[j].cursor_keys_mode != CURSOR_KEYS_DONTCARE &&
            info[j].cursor_keys_mode != term->cursor_keys_mode)
            continue;

        if (info[j].keypad_keys_mode != KEYPAD_DONTCARE &&
            info[j].keypad_keys_mode != term->keypad_keys_mode)
            continue;

        return &info[j];
    }

    return NULL;
}

static void
keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
             uint32_t time, uint32_t key, uint32_t state)
{
    struct wayland *wayl = data;
    struct terminal *term = wayl->kbd_focus;

    assert(term != NULL);

    const xkb_mod_mask_t ctrl = 1 << wayl->kbd.mod_ctrl;
    const xkb_mod_mask_t alt = 1 << wayl->kbd.mod_alt;
    const xkb_mod_mask_t shift = 1 << wayl->kbd.mod_shift;
    const xkb_mod_mask_t meta = 1 << wayl->kbd.mod_meta;

    if (state == XKB_KEY_UP) {
        stop_repeater(wayl, key);
        return;
    }

    key += 8;
    bool should_repeat = xkb_keymap_key_repeats(wayl->kbd.xkb_keymap, key);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(wayl->kbd.xkb_state, key);

#if 0
    char foo[100];
    xkb_keysym_get_name(sym, foo, sizeof(foo));
    LOG_INFO("%s", foo);
#endif

    xkb_compose_state_feed(wayl->kbd.xkb_compose_state, sym);
    enum xkb_compose_status compose_status = xkb_compose_state_get_status(
        wayl->kbd.xkb_compose_state);

    if (compose_status == XKB_COMPOSE_COMPOSING) {
        /* TODO: goto maybe_repeat? */
        return;
    }

    xkb_mod_mask_t mods = xkb_state_serialize_mods(
        wayl->kbd.xkb_state, XKB_STATE_MODS_DEPRESSED);
    //xkb_mod_mask_t consumed = xkb_state_key_get_consumed_mods(wayl->kbd.xkb_state, key);
    xkb_mod_mask_t consumed = 0x0;
    xkb_mod_mask_t significant = ctrl | alt | shift | meta;
    xkb_mod_mask_t effective_mods = mods & ~consumed & significant;

    if (term->is_searching) {
        if (should_repeat)
            start_repeater(wayl, key - 8);
        search_input(term, key, sym, effective_mods, serial);
        return;
    }

#if 0
    for (size_t i = 0; i < 32; i++) {
        if (mods & (1 << i)) {
            LOG_INFO("%s", xkb_keymap_mod_get_name(wayl->kbd.xkb_keymap, i));
        }
    }
#endif

    LOG_DBG("keyboard_key: keyboard=%p, serial=%u, "
            "sym=%u, mod=0x%08x, consumed=0x%08x, significant=0x%08x, "
            "effective=0x%08x, repeats=%d",
            wl_keyboard, serial,
            sym, mods, consumed, significant, effective_mods, should_repeat);

    /*
     * User configurable bindings
     */
    tll_foreach(wayl->kbd.bindings.key, it) {
        if (it->item.mods == effective_mods && it->item.sym == sym) {
            input_execute_binding(term, it->item.action, serial);
            goto maybe_repeat;
        }
    }

    /*
     * Keys generating escape sequences
     */

    enum modifier keymap_mods = MOD_NONE;
    keymap_mods |= wayl->kbd.shift ? MOD_SHIFT : MOD_NONE;
    keymap_mods |= wayl->kbd.alt ? MOD_ALT : MOD_NONE;
    keymap_mods |= wayl->kbd.ctrl ? MOD_CTRL : MOD_NONE;
    keymap_mods |= wayl->kbd.meta ? MOD_META : MOD_NONE;

    const struct key_data *keymap = keymap_lookup(term, sym, keymap_mods);
    if (keymap != NULL) {
        term_to_slave(term, keymap->seq, strlen(keymap->seq));

        term_reset_view(term);
        selection_cancel(term);
        goto maybe_repeat;
    }

    /*
     * Compose, and maybe emit "normal" character
     */

    uint8_t buf[64] = {0};
    int count = 0;

    if (compose_status == XKB_COMPOSE_COMPOSED) {
        count = xkb_compose_state_get_utf8(
            wayl->kbd.xkb_compose_state, (char *)buf, sizeof(buf));
        xkb_compose_state_reset(wayl->kbd.xkb_compose_state);
    } else if (compose_status == XKB_COMPOSE_CANCELLED) {
        goto maybe_repeat;
    } else {
        count = xkb_state_key_get_utf8(
            wayl->kbd.xkb_state, key, (char *)buf, sizeof(buf));
    }

    if (count == 0)
        goto maybe_repeat;

#define is_control_key(x) ((x) >= 0x40 && (x) <= 0x7f)
#define IS_CTRL(x) ((x) < 0x20 || ((x) >= 0x7f && (x) <= 0x9f))

    if ((keymap_mods & MOD_CTRL) &&
        !is_control_key(sym) &&
        (count == 1 && !IS_CTRL(buf[0])) &&
        sym < 256)
    {
        static const int mod_param_map[32] = {
            [MOD_SHIFT] = 2,
            [MOD_ALT] = 3,
            [MOD_SHIFT | MOD_ALT] = 4,
            [MOD_CTRL] = 5,
            [MOD_SHIFT | MOD_CTRL] = 6,
            [MOD_ALT | MOD_CTRL] = 7,
            [MOD_SHIFT | MOD_ALT | MOD_CTRL] = 8,
            [MOD_META] = 9,
            [MOD_META | MOD_SHIFT] = 10,
            [MOD_META | MOD_ALT] = 11,
            [MOD_META | MOD_SHIFT | MOD_ALT] = 12,
            [MOD_META | MOD_CTRL] = 13,
            [MOD_META | MOD_SHIFT | MOD_CTRL] = 14,
            [MOD_META | MOD_ALT | MOD_CTRL] = 15,
            [MOD_META | MOD_SHIFT | MOD_ALT | MOD_CTRL] = 16,
        };

        assert(keymap_mods < sizeof(mod_param_map) / sizeof(mod_param_map[0]));
        int modify_param = mod_param_map[keymap_mods];
        assert(modify_param != 0);

        char reply[1024];
        snprintf(reply, sizeof(reply), "\x1b[27;%d;%d~", modify_param, sym);
        term_to_slave(term, reply, strlen(reply));
    }

    else {
        if (effective_mods & alt) {
            /*
             * When the alt modifier is pressed, we do one out of three things:
             *
             *  1. we prefix the output bytes with ESC
             *  2. we set the 8:th bit in the output byte
             *  3. we ignore the alt modifier
             *
             * #1 is configured with \E[?1036, and is on by default
             *
             * If #1 has been disabled, we use #2, *if* it's a single
             * byte we're emitting. Since this is an UTF-8 terminal,
             * we then UTF8-encode the 8-bit character. #2 is
             * configured with \E[?1034, and is on by default.
             *
             * Lastly, if both #1 and #2 have been disabled, the alt
             * modifier is ignored.
             */
            if (term->meta.esc_prefix) {
                term_to_slave(term, "\x1b", 1);
                term_to_slave(term, buf, count);
            }

            else if (term->meta.eight_bit && count == 1) {
                const wchar_t wc = 0x80 | buf[0];

                char utf8[8];
                mbstate_t ps = {0};
                size_t chars = wcrtomb(utf8, wc, &ps);

                if (chars != (size_t)-1)
                    term_to_slave(term, utf8, chars);
                else
                    term_to_slave(term, buf, count);
            }

            else {
                /* Alt ignored */
                term_to_slave(term, buf, count);
            }
        } else
            term_to_slave(term, buf, count);
    }

    term_reset_view(term);
    selection_cancel(term);

maybe_repeat:
    clock_gettime(
        term->wl->presentation_clock_id, &term->render.input_time);

    if (should_repeat)
        start_repeater(wayl, key - 8);

}

static void
keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
                   uint32_t mods_depressed, uint32_t mods_latched,
                   uint32_t mods_locked, uint32_t group)
{
    struct wayland *wayl = data;

    LOG_DBG("modifiers: depressed=0x%x, latched=0x%x, locked=0x%x, group=%u",
            mods_depressed, mods_latched, mods_locked, group);

    xkb_state_update_mask(
        wayl->kbd.xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);

    /* Update state of modifiers we're interrested in for e.g mouse events */
    wayl->kbd.shift = xkb_state_mod_index_is_active(
        wayl->kbd.xkb_state, wayl->kbd.mod_shift, XKB_STATE_MODS_DEPRESSED);
    wayl->kbd.alt = xkb_state_mod_index_is_active(
        wayl->kbd.xkb_state, wayl->kbd.mod_alt, XKB_STATE_MODS_DEPRESSED);
    wayl->kbd.ctrl = xkb_state_mod_index_is_active(
        wayl->kbd.xkb_state, wayl->kbd.mod_ctrl, XKB_STATE_MODS_DEPRESSED);
    wayl->kbd.meta = xkb_state_mod_index_is_active(
        wayl->kbd.xkb_state, wayl->kbd.mod_meta, XKB_STATE_MODS_DEPRESSED);

    if (wayl->kbd_focus && wayl->kbd_focus->active_surface == TERM_SURF_GRID)
        term_xcursor_update(wayl->kbd_focus);
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                     int32_t rate, int32_t delay)
{
    struct wayland *wayl = data;
    LOG_DBG("keyboard repeat: rate=%d, delay=%d", rate, delay);
    wayl->kbd.repeat.rate = rate;
    wayl->kbd.repeat.delay = delay;
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
input_repeat(struct wayland *wayl, uint32_t key)
{
    keyboard_key(wayl, NULL, wayl->input_serial, 0, key, XKB_KEY_DOWN);
}

static bool
is_top_left(const struct terminal *term, int x, int y)
{
    int csd_border_size = term->conf->csd.border_width;
    return (
        (term->active_surface == TERM_SURF_BORDER_LEFT && y < 10 * term->scale) ||
        (term->active_surface == TERM_SURF_BORDER_TOP && x < (10 + csd_border_size) * term->scale));
}

static bool
is_top_right(const struct terminal *term, int x, int y)
{
    int csd_border_size = term->conf->csd.border_width;
    return (
        (term->active_surface == TERM_SURF_BORDER_RIGHT && y < 10 * term->scale) ||
        (term->active_surface == TERM_SURF_BORDER_TOP && x > term->width + 1 * csd_border_size * term->scale - 10 * term->scale));
}

static bool
is_bottom_left(const struct terminal *term, int x, int y)
{
    int csd_title_size = term->conf->csd.title_height;
    int csd_border_size = term->conf->csd.border_width;
    return (
        (term->active_surface == TERM_SURF_BORDER_LEFT && y > csd_title_size * term->scale + term->height) ||
        (term->active_surface == TERM_SURF_BORDER_BOTTOM && x < (10 + csd_border_size) * term->scale));
}

static bool
is_bottom_right(const struct terminal *term, int x, int y)
{
    int csd_title_size = term->conf->csd.title_height;
    int csd_border_size = term->conf->csd.border_width;
    return (
        (term->active_surface == TERM_SURF_BORDER_RIGHT && y > csd_title_size * term->scale + term->height) ||
        (term->active_surface == TERM_SURF_BORDER_BOTTOM && x > term->width + 1 * csd_border_size * term->scale - 10 * term->scale));
}

static const char *
xcursor_for_csd_border(struct terminal *term, int x, int y)
{
    if (is_top_left(term, x, y))                              return "top_left_corner";
    else if (is_top_right(term, x, y))                        return "top_right_corner";
    else if (is_bottom_left(term, x, y))                      return "bottom_left_corner";
    else if (is_bottom_right(term, x, y))                     return "bottom_right_corner";
    else if (term->active_surface == TERM_SURF_BORDER_LEFT)   return "left_side";
    else if (term->active_surface == TERM_SURF_BORDER_RIGHT)  return "right_side";
    else if (term->active_surface == TERM_SURF_BORDER_TOP)    return "top_side";
    else if (term->active_surface == TERM_SURF_BORDER_BOTTOM) return"bottom_side";
    else {
        assert(false);
        return NULL;
    }
}

static void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface,
                 wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    assert(surface != NULL);

    struct wayland *wayl = data;
    struct wl_window *win = wl_surface_get_user_data(surface);
    struct terminal *term = win->term;

    LOG_DBG("pointer-enter: pointer=%p, serial=%u, surface = %p, new-moused = %p",
            wl_pointer, serial, surface, term);

    wayl->mouse_focus = term;

    int x = wl_fixed_to_int(surface_x) * term->scale;
    int y = wl_fixed_to_int(surface_y) * term->scale;

    switch ((term->active_surface = term_surface_kind(term, surface))) {
    case TERM_SURF_GRID:
        wayl->mouse.col = x / term->cell_width;
        wayl->mouse.row = y / term->cell_height;
        term_xcursor_update(term);
        break;

    case TERM_SURF_SEARCH:
    case TERM_SURF_TITLE:
        term->xcursor = "left_ptr";
        render_xcursor_set(term);
        break;

    case TERM_SURF_BORDER_LEFT:
    case TERM_SURF_BORDER_RIGHT:
    case TERM_SURF_BORDER_TOP:
    case TERM_SURF_BORDER_BOTTOM:
        term->xcursor = xcursor_for_csd_border(term, x, y);
        render_xcursor_set(term);
        break;

    case TERM_SURF_BUTTON_MINIMIZE:
    case TERM_SURF_BUTTON_MAXIMIZE:
    case TERM_SURF_BUTTON_CLOSE:
        term->xcursor = "left_ptr";
        render_xcursor_set(term);
        render_refresh_csd(term);
        break;

    case TERM_SURF_NONE:
        assert(false);
        break;
    }
}

static void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface)
{
    struct wayland *wayl = data;
    struct terminal *old_moused = wayl->mouse_focus;

    LOG_DBG(
        "pointer-leave: pointer=%p, serial=%u, surface = %p, old-moused = %p",
        wl_pointer, serial, surface, old_moused);

    if (wayl->pointer.xcursor_callback != NULL) {
        /* A cursor frame callback may never be called if the pointer leaves our surface */
        wl_callback_destroy(wayl->pointer.xcursor_callback);
        wayl->pointer.xcursor_callback = NULL;
        wayl->pointer.pending_terminal = NULL;
        wayl->pointer.xcursor = NULL;
    }

    /* Reset mouse state */
    memset(&wayl->mouse, 0, sizeof(wayl->mouse));

    wayl->mouse_focus = NULL;
    if (old_moused == NULL) {
        LOG_WARN(
            "compositor sent pointer_leave event without a pointer_enter "
            "event: surface=%p", surface);
    } else {
        if (surface != NULL) {
            /* Sway 1.4 sends this event with a NULL surface when we destroy the window */
            const struct wl_window *win __attribute__((unused))
                = wl_surface_get_user_data(surface);
            assert(old_moused == win->term);
        }

        enum term_surface active_surface = old_moused->active_surface;

        old_moused->active_surface = TERM_SURF_NONE;
        term_xcursor_update(old_moused);

        switch (active_surface) {
        case TERM_SURF_BUTTON_MINIMIZE:
        case TERM_SURF_BUTTON_MAXIMIZE:
        case TERM_SURF_BUTTON_CLOSE:
            if (old_moused->is_shutting_down)
                break;

            render_refresh_csd(old_moused);
            break;

        case TERM_SURF_NONE:
        case TERM_SURF_GRID:
        case TERM_SURF_SEARCH:
        case TERM_SURF_TITLE:
        case TERM_SURF_BORDER_LEFT:
        case TERM_SURF_BORDER_RIGHT:
        case TERM_SURF_BORDER_TOP:
        case TERM_SURF_BORDER_BOTTOM:
            break;
        }

    }
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
                  uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct wayland *wayl = data;
    struct terminal *term = wayl->mouse_focus;
    struct wl_window *win = term->window;

    LOG_DBG("pointer_motion: pointer=%p, x=%d, y=%d", wl_pointer,
            wl_fixed_to_int(surface_x), wl_fixed_to_int(surface_y));

    assert(term != NULL);

    int x = wl_fixed_to_int(surface_x) * term->scale;
    int y = wl_fixed_to_int(surface_y) * term->scale;

    wayl->mouse.x = x;
    wayl->mouse.y = y;

    switch (term->active_surface) {
    case TERM_SURF_NONE:
    case TERM_SURF_SEARCH:
    case TERM_SURF_BUTTON_MINIMIZE:
    case TERM_SURF_BUTTON_MAXIMIZE:
    case TERM_SURF_BUTTON_CLOSE:
        break;

    case TERM_SURF_TITLE:
        /* We've started a 'move' timer, but user started dragging
         * right away - abort the timer and initiate the actual move
         * right away */
        if (wayl->mouse.button == BTN_LEFT && win->csd.move_timeout_fd != -1) {
            fdm_del(wayl->fdm, win->csd.move_timeout_fd);
            win->csd.move_timeout_fd = -1;
            xdg_toplevel_move(win->xdg_toplevel, wayl->seat, win->csd.serial);
        }
        break;

    case TERM_SURF_BORDER_LEFT:
    case TERM_SURF_BORDER_RIGHT:
    case TERM_SURF_BORDER_TOP:
    case TERM_SURF_BORDER_BOTTOM:
        term->xcursor = xcursor_for_csd_border(term, x, y);
        render_xcursor_set(term);
        break;

    case TERM_SURF_GRID: {
        int col = (x - term->margins.left) / term->cell_width;
        int row = (y - term->margins.top) / term->cell_height;

        if (col < 0 || row < 0 || col >= term->cols || row >= term->rows)
            return;

        bool update_selection = wayl->mouse.button == BTN_LEFT;
        bool update_selection_early = term->selection.end.row == -1;

        if (update_selection && update_selection_early)
            selection_update(term, col, row);

        if (col == wayl->mouse.col && row == wayl->mouse.row)
            break;

        wayl->mouse.col = col;
        wayl->mouse.row = row;

        if (update_selection && !update_selection_early)
            selection_update(term, col, row);

        term_mouse_motion(
        term, wayl->mouse.button, wayl->mouse.row, wayl->mouse.col);
        break;
    }
    }
}

static bool
fdm_csd_move(struct fdm *fdm, int fd, int events, void *data)
{
    struct wl_window *win = data;
    struct wayland *wayl = win->term->wl;

    fdm_del(fdm, fd);
    win->csd.move_timeout_fd = -1;

    xdg_toplevel_move(win->xdg_toplevel, wayl->seat, win->csd.serial);
    return true;
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
                  uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    LOG_DBG("BUTTON: pointer=%p, serial=%u, button=%x, state=%u",
            wl_pointer, serial, button, state);

    struct wayland *wayl = data;
    struct terminal *term = wayl->mouse_focus;

    assert(term != NULL);

    /* Update double/triple click state */
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        /* Time since last click */
        struct timeval now, since_last;
        gettimeofday(&now, NULL);
        timersub(&now, &wayl->mouse.last_time, &since_last);

        /* Double- or triple click? */
        if (button == wayl->mouse.last_button &&
            since_last.tv_sec == 0 &&
            since_last.tv_usec <= 300 * 1000)
        {
            wayl->mouse.count++;
        } else
            wayl->mouse.count = 1;

        wayl->mouse.button = button; /* For motion events */
        wayl->mouse.last_button = button;
        wayl->mouse.last_time = now;
    } else
        wayl->mouse.button = 0; /* For motion events */

    switch (term->active_surface) {
    case TERM_SURF_TITLE:
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {

            struct wl_window *win = term->window;

            /* Toggle maximized state on double-click */
            if (button == BTN_LEFT && wayl->mouse.count == 2) {
                if (win->is_maximized)
                    xdg_toplevel_unset_maximized(win->xdg_toplevel);
                else
                    xdg_toplevel_set_maximized(win->xdg_toplevel);
            }

            else if (button == BTN_LEFT && win->csd.move_timeout_fd == -1) {
                const struct itimerspec timeout = {
                    .it_value = {.tv_nsec = 200000000},
                };

                int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
                if (fd >= 0 &&
                    timerfd_settime(fd, 0, &timeout, NULL) == 0 &&
                    fdm_add(wayl->fdm, fd, EPOLLIN, &fdm_csd_move, win))
                {
                    win->csd.move_timeout_fd = fd;
                    win->csd.serial = serial;
                } else {
                    LOG_ERRNO("failed to configure XDG toplevel move timer FD");
                    close(fd);
                }
            }
        }

        else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
            struct wl_window *win = term->window;
            if (win->csd.move_timeout_fd != -1) {
                fdm_del(wayl->fdm, win->csd.move_timeout_fd);
                win->csd.move_timeout_fd = -1;
            }
        }
        return;

    case TERM_SURF_BORDER_LEFT:
    case TERM_SURF_BORDER_RIGHT:
    case TERM_SURF_BORDER_TOP:
    case TERM_SURF_BORDER_BOTTOM: {
        static const enum xdg_toplevel_resize_edge map[] = {
            [TERM_SURF_BORDER_LEFT] = XDG_TOPLEVEL_RESIZE_EDGE_LEFT,
            [TERM_SURF_BORDER_RIGHT] = XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,
            [TERM_SURF_BORDER_TOP] = XDG_TOPLEVEL_RESIZE_EDGE_TOP,
            [TERM_SURF_BORDER_BOTTOM] = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,
        };

        if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
            enum xdg_toplevel_resize_edge resize_type;

            int x = wayl->mouse.x;
            int y = wayl->mouse.y;

            if (is_top_left(term, x, y))
                resize_type = XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
            else if (is_top_right(term, x, y))
                resize_type = XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
            else if (is_bottom_left(term, x, y))
                resize_type = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
            else if (is_bottom_right(term, x, y))
                resize_type = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
            else
                resize_type = map[term->active_surface];

            xdg_toplevel_resize(
                term->window->xdg_toplevel, term->wl->seat, serial, resize_type);
        }
        return;
    }

    case TERM_SURF_BUTTON_MINIMIZE:
        if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED)
            xdg_toplevel_set_minimized(term->window->xdg_toplevel);
        break;

    case TERM_SURF_BUTTON_MAXIMIZE:
        if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
            if (term->window->is_maximized)
                xdg_toplevel_unset_maximized(term->window->xdg_toplevel);
            else
                xdg_toplevel_set_maximized(term->window->xdg_toplevel);
        }
        break;

    case TERM_SURF_BUTTON_CLOSE:
        if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED)
            term_shutdown(term);
        break;

    case TERM_SURF_SEARCH:
        break;

    case TERM_SURF_GRID: {
        search_cancel(term);

        switch (state) {
        case WL_POINTER_BUTTON_STATE_PRESSED: {
            if (button == BTN_LEFT) {
                switch (wayl->mouse.count) {
                case 1:
                    selection_start(
                        term, wayl->mouse.col, wayl->mouse.row,
                        wayl->kbd.ctrl ? SELECTION_BLOCK : SELECTION_NORMAL);
                    break;

                case 2:
                    selection_mark_word(term, wayl->mouse.col, wayl->mouse.row,
                                        wayl->kbd.ctrl, serial);
                    break;

                case 3:
                    selection_mark_row(term, wayl->mouse.row, serial);
                    break;
                }
            }

            else {
                for (size_t i = 0; i < ALEN(wayl->conf->bindings.mouse); i++) {
                    const struct mouse_binding *binding =
                        &wayl->conf->bindings.mouse[i];

                    if (binding->button != button) {
                        /* Wrong button */
                        continue;
                    }

                    if  (binding->count != wayl->mouse.count) {
                        /* Not correct click count */
                        continue;
                    }

                    input_execute_binding(term, binding->action, serial);
                    break;
                }
                selection_cancel(term);
            }

            term_mouse_down(term, button, wayl->mouse.row, wayl->mouse.col);
            break;
        }

        case WL_POINTER_BUTTON_STATE_RELEASED:
            if (button != BTN_LEFT || term->selection.end.col == -1)
                selection_cancel(term);
            else
                selection_finalize(term, serial);

            term_mouse_up(term, button, wayl->mouse.row, wayl->mouse.col);
            break;
        }
        break;
    }

    case TERM_SURF_NONE:
        assert(false);
        break;

    }
}

static void
mouse_scroll(struct wayland *wayl, int amount)
{
    struct terminal *term = wayl->mouse_focus;
    assert(term != NULL);

    int button = amount < 0 ? BTN_BACK : BTN_FORWARD;

    void (*scrollback)(struct terminal *term, int rows)
        = amount < 0 ? &cmd_scrollback_up : &cmd_scrollback_down;

    amount = abs(amount);

    if ((button == BTN_BACK || button == BTN_FORWARD) &&
        term->grid == &term->alt && term->alt_scrolling)
    {
        /*
         * alternateScroll/faux scrolling - translate mouse
         * "back"/"forward" to up/down keys
         */

        static xkb_keycode_t key_arrow_up = 0;
        static xkb_keycode_t key_arrow_down = 0;

        if (key_arrow_up == 0) {
            key_arrow_up = xkb_keymap_key_by_name(wayl->kbd.xkb_keymap, "UP");
            key_arrow_down = xkb_keymap_key_by_name(wayl->kbd.xkb_keymap, "DOWN");
        }

        xkb_keycode_t key = button == BTN_BACK ? key_arrow_up : key_arrow_down;

        for (int i = 0; i < amount; i++)
            keyboard_key(wayl, NULL, wayl->input_serial, 0, key - 8, XKB_KEY_DOWN);
        keyboard_key(wayl, NULL, wayl->input_serial, 0, key - 8, XKB_KEY_UP);
    } else {
        for (int i = 0; i < amount; i++)
            term_mouse_down(term, button, wayl->mouse.row, wayl->mouse.col);
        term_mouse_up(term, button, wayl->mouse.row, wayl->mouse.col);

        scrollback(term, amount);
    }
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
                uint32_t time, uint32_t axis, wl_fixed_t value)
{
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    struct wayland *wayl = data;

    if (wayl->mouse.have_discrete)
        return;

    mouse_scroll(wayl, wl_fixed_to_int(value));
}

static void
wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                         uint32_t axis, int32_t discrete)
{
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    struct wayland *wayl = data;
    wayl->mouse.have_discrete = true;
    mouse_scroll(wayl, discrete);
}

static void
wl_pointer_frame(void *data, struct wl_pointer *wl_pointer)
{
    struct wayland *wayl = data;
    wayl->mouse.have_discrete = false;
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
