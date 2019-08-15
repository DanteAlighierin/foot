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
    if (term->kbd.xkb_compose_state != NULL) {
        xkb_compose_state_unref(term->kbd.xkb_compose_state);
        term->kbd.xkb_compose_state = NULL;
    }
    if (term->kbd.xkb_compose_table != NULL) {
        xkb_compose_table_unref(term->kbd.xkb_compose_table);
        term->kbd.xkb_compose_table = NULL;
    }
    if (term->kbd.xkb_keymap != NULL) {
        xkb_keymap_unref(term->kbd.xkb_keymap);
        term->kbd.xkb_keymap = NULL;
    }
    if (term->kbd.xkb_state != NULL) {
        xkb_state_unref(term->kbd.xkb_state);
        term->kbd.xkb_state = NULL;
    }
    if (term->kbd.xkb != NULL) {
        xkb_context_unref(term->kbd.xkb);
        term->kbd.xkb = NULL;
    }

    term->kbd.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    term->kbd.xkb_keymap = xkb_keymap_new_from_string(
        term->kbd.xkb, map_str, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    /* TODO: initialize in enter? */
    term->kbd.xkb_state = xkb_state_new(term->kbd.xkb_keymap);

    term->kbd.mod_shift = xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Shift");
    term->kbd.mod_alt = xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Mod1") ;
    term->kbd.mod_ctrl = xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Control");
    term->kbd.mod_meta = xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Mod4");

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
    struct terminal *term = data;
    term->input_serial = serial;
    term_focus_in(term);
}

static bool
start_repeater(struct terminal *term, uint32_t key)
{
    if (term->kbd.repeat.dont_re_repeat)
        return true;

    struct itimerspec t = {
        .it_value = {.tv_sec = 0, .tv_nsec = term->kbd.repeat.delay * 1000000},
        .it_interval = {.tv_sec = 0, .tv_nsec = 1000000000 / term->kbd.repeat.rate},
    };

    if (t.it_value.tv_nsec >= 1000000000) {
        t.it_value.tv_sec += t.it_value.tv_nsec / 1000000000;
        t.it_value.tv_nsec %= 1000000000;
    }
    if (t.it_interval.tv_nsec >= 1000000000) {
        t.it_interval.tv_sec += t.it_interval.tv_nsec / 1000000000;
        t.it_interval.tv_nsec %= 1000000000;
    }
    if (timerfd_settime(term->kbd.repeat.fd, 0, &t, NULL) < 0) {
        LOG_ERRNO("failed to arm keyboard repeat timer");
        return false;
    }

    term->kbd.repeat.key = key;
    return true;
}

static bool
stop_repeater(struct terminal *term, uint32_t key)
{
    if (key != -1 && key != term->kbd.repeat.key)
        return true;

    if (timerfd_settime(term->kbd.repeat.fd, 0, &(struct itimerspec){{0}}, NULL) < 0) {
        LOG_ERRNO("failed to disarm keyboard repeat timer");
        return false;
    }

    return true;
}

static void
keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
               struct wl_surface *surface)
{
    struct terminal *term = data;

    stop_repeater(term, -1);
    term_focus_out(term);
}

static void
keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
             uint32_t time, uint32_t key, uint32_t state)
{
    struct terminal *term = data;

    const xkb_mod_mask_t ctrl = 1 << term->kbd.mod_ctrl;
    const xkb_mod_mask_t alt = 1 << term->kbd.mod_alt;
    const xkb_mod_mask_t shift = 1 << term->kbd.mod_shift;
    const xkb_mod_mask_t meta = 1 << term->kbd.mod_meta;

    if (state == XKB_KEY_UP) {
        stop_repeater(term, key);
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
    xkb_mod_mask_t significant = ctrl | alt | shift | meta;
    xkb_mod_mask_t effective_mods = mods & ~consumed & significant;

#if 0
    for (size_t i = 0; i < 32; i++) {
        if (mods & (1 << i)) {
            LOG_INFO("%s", xkb_keymap_mod_get_name(term->kbd.xkb_keymap, i));
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
    keymap_mods |= term->kbd.meta ? MOD_META : MOD_NONE;

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
        uint8_t buf[64] = {0};
        int count = 0;

        if (compose_status == XKB_COMPOSE_COMPOSED) {
            count = xkb_compose_state_get_utf8(
                term->kbd.xkb_compose_state, (char *)buf, sizeof(buf));
            xkb_compose_state_reset(term->kbd.xkb_compose_state);
        } else {
            count = xkb_state_key_get_utf8(
                term->kbd.xkb_state, key, (char *)buf, sizeof(buf));
        }

        if (count > 0) {

#define is_control_key(x) ((x) >= 0x40 && (x) <= 0x7f)
#define IS_CTRL(x) ((x) < 0x20 || ((x) >= 0x7f && (x) <= 0x9f))

            if ((keymap_mods & MOD_CTRL) &&
                !is_control_key(sym) &&
                (count == 1 && !IS_CTRL(buf[0])) &&
                sym < 256)
            {
                static const int mod_param_map[16] = {
                    [MOD_SHIFT] = 2,
                    [MOD_ALT] = 3,
                    [MOD_SHIFT | MOD_ALT] = 4,
                    [MOD_CTRL] = 5,
                    [MOD_SHIFT | MOD_CTRL] = 6,
                    [MOD_ALT | MOD_CTRL] = 7,
                    [MOD_SHIFT | MOD_ALT | MOD_CTRL] = 8,
                };
                int modify_param = mod_param_map[keymap_mods];
                assert(modify_param != 0);

                char reply[1024];
                snprintf(reply, sizeof(reply), "\x1b[27;%d;%d~", modify_param, sym);
                vt_to_slave(term, reply, strlen(reply));
            }

            else {
                if (effective_mods & alt)
                    vt_to_slave(term, "\x1b", 1);

                vt_to_slave(term, buf, count);

                if (term->grid->view != term->grid->offset) {
                    term->grid->view = term->grid->offset;
                    term_damage_all(term);
                }
            }

            selection_cancel(term);
        }
    }

    start_repeater(term, key - 8);
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
    term->kbd.meta = xkb_state_mod_index_is_active(
        term->kbd.xkb_state, term->kbd.mod_meta, XKB_STATE_MODS_DEPRESSED);
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
    case WL_POINTER_BUTTON_STATE_PRESSED: {
        /* Time since last click */
        struct timeval now, since_last;
        gettimeofday(&now, NULL);
        timersub(&now, &term->mouse.last_time, &since_last);

        /* Double- or triple click? */
        if (button == term->mouse.last_button &&
            since_last.tv_sec == 0 &&
            since_last.tv_usec <= 300 * 1000)
        {
            term->mouse.count++;
        } else
            term->mouse.count = 1;

        if (button == BTN_LEFT) {
            switch (term->mouse.count) {
            case 1:
                selection_start(term, term->mouse.col, term->mouse.row);
                break;

            case 2:
                selection_mark_word(term, term->mouse.col, term->mouse.row,
                                    term->kbd.ctrl, serial);
                break;

            case 3:
                selection_mark_row(term, term->mouse.row, serial);
                break;
            }
        } else {
            if (term->mouse.count == 1 && button == BTN_MIDDLE && selection_enabled(term))
                selection_from_primary(term);
            selection_cancel(term);
        }

        term->mouse.button = button; /* For motion events */
        term->mouse.last_button = button;
        term->mouse.last_time = now;
        term_mouse_down(term, button, term->mouse.row, term->mouse.col,
                        term->kbd.shift, term->kbd.alt, term->kbd.ctrl);
        break;
    }

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
mouse_scroll(struct terminal *term, int amount)
{
    int button = amount < 0 ? BTN_BACK : BTN_FORWARD;

    void (*scrollback)(struct terminal *term, int rows)
        = amount < 0 ? &cmd_scrollback_up : &cmd_scrollback_down;

    amount = abs(amount);

    for (int i = 0; i < amount; i++)
        term_mouse_down(term, button, term->mouse.row, term->mouse.col,
                        term->kbd.shift, term->kbd.alt, term->kbd.ctrl);

    scrollback(term, amount);
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
                uint32_t time, uint32_t axis, wl_fixed_t value)
{
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    struct terminal *term = data;
    if (term->mouse.have_discrete)
        return;

    mouse_scroll(term, wl_fixed_to_int(value));
}

static void
wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                         uint32_t axis, int32_t discrete)
{
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    struct terminal *term = data;
    term->mouse.have_discrete = true;
    mouse_scroll(term, discrete);
}

static void
wl_pointer_frame(void *data, struct wl_pointer *wl_pointer)
{
    struct terminal *term = data;
    term->mouse.have_discrete = false;
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
