#include "input.h"

#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <threads.h>
#include <locale.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/timerfd.h>

#include <linux/input-event-codes.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "input"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "commands.h"
#include "keymap.h"
#include "render.h"
#include "search.h"
#include "selection.h"
#include "terminal.h"
#include "vt.h"

static void
keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                uint32_t format, int32_t fd, uint32_t size)
{
    struct wayland *wayl = data;

    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

    /* TODO: free old context + keymap */
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
}

static void
keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
               struct wl_surface *surface, struct wl_array *keys)
{
    assert(surface != NULL);

    struct wayland *wayl = data;
    wayl->input_serial = serial;
    wayl->focused = wayl_terminal_from_surface(wayl, surface);
    assert(wayl->focused != NULL);

    term_focus_in(wayl->focused);
    term_xcursor_update(wayl->focused);
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

    assert(
        wayl->focused == NULL ||
        surface == NULL ||  /* Seen on Sway 1.2 */
        wayl_terminal_from_surface(wayl, surface) == wayl->focused);

    struct terminal *old_focused = wayl->focused;
    wayl->focused = NULL;

    stop_repeater(wayl, -1);
    if (old_focused != NULL) {
        /*
         * Sway bug - under certain conditions we get a
         * keyboard_leave() (and keyboard_key()) without first having
         * received a keyboard_enter()
         */
        term_focus_out(old_focused);
        term_xcursor_update(old_focused);
    } else {
        LOG_WARN(
            "compositor sent keyboard_leave event without a keyboard_enter "
            "event: surface=%p", surface);
    }
}

static void
keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
             uint32_t time, uint32_t key, uint32_t state)
{
    struct wayland *wayl = data;
    struct terminal *term = wayl->focused;

    /* Workaround buggy Sway 1.2 */
    if (term == NULL) {
        static bool have_warned = false;
        if (!have_warned) {
            have_warned = true;
            LOG_WARN("compositor sent keyboard_key event without first sending keyboard_enter");
        }

        if (tll_length(wayl->terms) == 1) {
            /* With only one terminal we *know* which one has focus */
            term = tll_front(wayl->terms);
        } else {
            /* But with multiple windows we can't guess - ignore the event */
            stop_repeater(wayl, -1);
            return;
        }
    }

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
        search_input(term, key, sym, effective_mods);
        return;
    }

#if 0
    for (size_t i = 0; i < 32; i++) {
        if (mods & (1 << i)) {
            LOG_INFO("%s", xkb_keymap_mod_get_name(wayl->kbd.xkb_keymap, i));
        }
    }
#endif

    LOG_DBG("sym=%u, mod=0x%08x, consumed=0x%08x, significant=0x%08x, "
            "effective=0x%08x, repeats=%d",
            sym, mods, consumed, significant, effective_mods, should_repeat);

    enum modifier keymap_mods = MOD_NONE;
    keymap_mods |= wayl->kbd.shift ? MOD_SHIFT : MOD_NONE;
    keymap_mods |= wayl->kbd.alt ? MOD_ALT : MOD_NONE;
    keymap_mods |= wayl->kbd.ctrl ? MOD_CTRL : MOD_NONE;
    keymap_mods |= wayl->kbd.meta ? MOD_META : MOD_NONE;

    if (effective_mods == shift) {
        if (sym == XKB_KEY_Page_Up) {
            cmd_scrollback_up(term, term->rows);
            goto maybe_repeat;
        }

        else if (sym == XKB_KEY_Page_Down) {
            cmd_scrollback_down(term, term->rows);
            goto maybe_repeat;
        }
    }

    else if (effective_mods == (shift | ctrl)) {
        if (sym == XKB_KEY_C) {
            selection_to_clipboard(term, serial);
            goto maybe_repeat;
        }

        else if (sym == XKB_KEY_V) {
            selection_from_clipboard(term, serial);
            term_reset_view(term);
            goto maybe_repeat;
        }

        else if (sym == XKB_KEY_R) {
            search_begin(term);
            goto maybe_repeat;
        }
    }


    for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
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

            term_to_slave(term, info->seq, strlen(info->seq));

            term_reset_view(term);
            selection_cancel(term);
            goto maybe_repeat;
        }
    }

    uint8_t buf[64] = {0};
    int count = 0;

    if (compose_status == XKB_COMPOSE_COMPOSED) {
        count = xkb_compose_state_get_utf8(
            wayl->kbd.xkb_compose_state, (char *)buf, sizeof(buf));
        xkb_compose_state_reset(wayl->kbd.xkb_compose_state);
    } else {
        count = xkb_state_key_get_utf8(
            wayl->kbd.xkb_state, key, (char *)buf, sizeof(buf));
    }

    if (count > 0) {

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
            if (effective_mods & alt)
                term_to_slave(term, "\x1b", 1);

            term_to_slave(term, buf, count);
        }

        term_reset_view(term);
        selection_cancel(term);
    }

maybe_repeat:
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

    if (wayl->focused)
        term_xcursor_update(wayl->focused);
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

static void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface,
                 wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    assert(surface != NULL);

    struct wayland *wayl = data;
    struct terminal *term = wayl_terminal_from_surface(wayl, surface);

    LOG_DBG("pointer-enter: surface = %p, new-moused = %p", surface, term);

    wayl->moused = term;

    int x = wl_fixed_to_int(surface_x) * term->scale;
    int y = wl_fixed_to_int(surface_y) * term->scale;

    wayl->mouse.col = x / term->cell_width;
    wayl->mouse.row = y / term->cell_height;

    term_xcursor_update(term);
}

static void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface)
{
    struct wayland *wayl = data;
    struct terminal *old_moused = wayl->moused;

    LOG_DBG("pointer-leave: surface = %p, old-moused = %p", surface, old_moused);

    wayl->moused = NULL;
    if (old_moused == NULL) {
        LOG_WARN(
            "compositor sent pointer_leave event without a pointer_enter "
            "event: surface=%p", surface);
    } else
        term_xcursor_update(old_moused);
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
                  uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct wayland *wayl = data;
    struct terminal *term = wayl->moused;

    /* Workaround buggy Sway 1.2 */
    if (term == NULL) {
        static bool have_warned = false;
        if (!have_warned) {
            have_warned = true;
            LOG_WARN("compositor sent pointer_motion event without first sending pointer_enter");
        }

        if (tll_length(wayl->terms) == 1) {
            /* With only one terminal we *know* which one has focus */
            term = tll_front(wayl->terms);
        } else {
            /* But with multiple windows we can't guess - ignore the event */
            return;
        }
    }

    assert(term != NULL);

    int x = wl_fixed_to_int(surface_x) * term->scale;
    int y = wl_fixed_to_int(surface_y) * term->scale;

    int col = (x - term->x_margin) / term->cell_width;
    int row = (y - term->y_margin) / term->cell_height;

    if (col < 0 || row < 0 || col >= term->cols || row >= term->rows)
        return;

    bool update_selection = wayl->mouse.button == BTN_LEFT;
    bool update_selection_early = term->selection.end.row == -1;

    if (update_selection && update_selection_early)
        selection_update(term, col, row);

    if (col == wayl->mouse.col && row == wayl->mouse.row)
        return;

    wayl->mouse.col = col;
    wayl->mouse.row = row;

    if (update_selection && !update_selection_early)
        selection_update(term, col, row);

    term_mouse_motion(
        term, wayl->mouse.button, wayl->mouse.row, wayl->mouse.col);
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
                  uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    LOG_DBG("BUTTON: button=%x, state=%u", button, state);

    struct wayland *wayl = data;
    struct terminal *term = wayl->moused;

    /* Workaround buggy Sway 1.2 */
    if (term == NULL) {
        static bool have_warned = false;
        if (!have_warned) {
            have_warned = true;
            LOG_WARN("compositor sent pointer_button event without first sending pointer_enter");
        }

        if (tll_length(wayl->terms) == 1) {
            /* With only one terminal we *know* which one has focus */
            term = tll_front(wayl->terms);
        } else {
            /* But with multiple windows we can't guess - ignore the event */
            return;
        }
    }

    assert(term != NULL);
    search_cancel(term);

    switch (state) {
    case WL_POINTER_BUTTON_STATE_PRESSED: {
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

        if (button == BTN_LEFT) {
            switch (wayl->mouse.count) {
            case 1:
                selection_start(term, wayl->mouse.col, wayl->mouse.row);
                break;

            case 2:
                selection_mark_word(term, wayl->mouse.col, wayl->mouse.row,
                                    wayl->kbd.ctrl, serial);
                break;

            case 3:
                selection_mark_row(term, wayl->mouse.row, serial);
                break;
            }
        } else {
            if (wayl->mouse.count == 1 && button == BTN_MIDDLE && selection_enabled(term))
                selection_from_primary(term);
            selection_cancel(term);
        }

        wayl->mouse.button = button; /* For motion events */
        wayl->mouse.last_button = button;
        wayl->mouse.last_time = now;
        term_mouse_down(term, button, wayl->mouse.row, wayl->mouse.col);
        break;
    }

    case WL_POINTER_BUTTON_STATE_RELEASED:
        if (button != BTN_LEFT || term->selection.end.col == -1)
            selection_cancel(term);
        else
            selection_finalize(term, serial);

        wayl->mouse.button = 0; /* For motion events */
        term_mouse_up(term, button, wayl->mouse.row, wayl->mouse.col);
        break;
    }
}

static void
mouse_scroll(struct wayland *wayl, int amount)
{
    struct terminal *term = wayl->moused;
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
