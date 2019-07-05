#include "input.h"

#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <threads.h>
#include <sys/mman.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define LOG_MODULE "input"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "terminal.h"
#include "render.h"

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
}

static void
keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
               struct wl_surface *surface)
{
    struct terminal *term = data;

    mtx_lock(&term->kbd.repeat.mutex);
    if (term->kbd.repeat.cmd != REPEAT_EXIT) {
        term->kbd.repeat.cmd = REPEAT_STOP;
        cnd_signal(&term->kbd.repeat.cond);
    }
    mtx_unlock(&term->kbd.repeat.mutex);
}

struct keymap {
    const char *normal;
    const char *shift;
    const char *alt;
    const char *shift_alt;
    const char *ctrl;
    const char *shift_ctrl;
    const char *alt_ctrl;
    const char *shift_alt_ctrl;
};

static const struct keymap key_map[][2] = {
    [XKB_KEY_Up] = {
        {"[A", "[1;2A", "[1;3A", "[1;4A", "[1;5A", "[1;6A", "[1;7A", "[1;8A"},
        {"OA", "[1;2A", "[1;3A", "[1;4A", "[1;5A", "[1;6A", "[1;7A", "[1;8A"}},
    [XKB_KEY_Down] = {
        {"[B", "[1;2B", "[1;3B", "[1;4B", "[1;5B", "[1;6B", "[1;7B", "[1;8B"},
        {"OB", "[1;2B", "[1;3B", "[1;4B", "[1;5B", "[1;6B", "[1;7B", "[1;8B"}},
    [XKB_KEY_Right] = {
        {"[C", "[1;2C", "[1;3C", "[1;4C", "[1;5C", "[1;6C", "[1;7C", "[1;8C"},
        {"OC", "[1;2C", "[1;3C", "[1;4C", "[1;5C", "[1;6C", "[1;7C", "[1;8C"}},
    [XKB_KEY_Left] = {
        {"[D", "[1;2D", "[1;3D", "[1;4D", "[1;5D", "[1;6D", "[1;7D", "[1;8D"},
        {"OD", "[1;2D", "[1;3D", "[1;4D", "[1;5D", "[1;6D", "[1;7D", "[1;8D"}},
    [XKB_KEY_Home] = {
        {"[H", "[1;2H", "[1;3H", "[1;4H", "[1;5H", "[1;6H", "[1;7H", "[1;8H"},
        {"OH", "[1;2H", "[1;3H", "[1;4H", "[1;5H", "[1;6H", "[1;7H", "[1;8H"}},
    [XKB_KEY_End] = {
        {"[F", "[1;2F", "[1;3F", "[1;4F", "[1;5F", "[1;6F", "[1;7F", "[1;8F"},
        {"OF", "[1;2F", "[1;3F", "[1;4F", "[1;5F", "[1;6F", "[1;7F", "[1;8F"}},
    [XKB_KEY_Insert] = {
        {"[2~", "[2;2~", "[2;3~", "[2;4~", "[2;5~", "[2;6~", "[2;7~", "[2;8~"},
        {"[2~", "[2;2~", "[2;3~", "[2;4~", "[2;5~", "[2;6~", "[2;7~", "[2;8~"}},
    [XKB_KEY_Delete] =  {
        {"[3~", "[3;2~", "[3;3~", "[3;4~", "[3;5~", "[3;6~", "[3;7~", "[3;8~"},
        {"[3~", "[3;2~", "[3;3~", "[3;4~", "[3;5~", "[3;6~", "[3;7~", "[3;8~"}},
    [XKB_KEY_Page_Up] = {
        {"[5~", "[5;2~", "[5;3~", "[5;4~", "[5;5~", "[5;6~", "[5;7~", "[5;8~"},
        {"[5~", "[5;2~", "[5;3~", "[5;4~", "[5;5~", "[5;6~", "[5;7~", "[5;8~"}},
    [XKB_KEY_Page_Down] = {
        {"[6~", "[6;2~", "[6;3~", "[6;4~", "[6;5~", "[6;6~", "[6;7~", "[6;8~"},
        {"[6~", "[6;2~", "[6;3~", "[6;4~", "[6;5~", "[6;6~", "[6;7~", "[6;8~"}},
    [XKB_KEY_F1] = {
        {"OP", "[1;2P", "[1;3P", "[1;4P", "[1;5P", "[1;6P", "[1;7P", "[1;8P"},
        {"OP", "[1;2P", "[1;3P", "[1;4P", "[1;5P", "[1;6P", "[1;7P", "[1;8P"}},
    [XKB_KEY_F2] = {
        {"OQ", "[1;2Q", "[1;3Q", "[1;4Q", "[1;5Q", "[1;6Q", "[1;7Q", "[1;8Q"},
        {"OQ", "[1;2Q", "[1;3Q", "[1;4Q", "[1;5Q", "[1;6Q", "[1;7Q", "[1;8Q"}},
    [XKB_KEY_F3] = {
        {"OR", "[1;2R", "[1;3R", "[1;4R", "[1;5R", "[1;6R", "[1;7R", "[1;8R"},
        {"OR", "[1;2R", "[1;3R", "[1;4R", "[1;5R", "[1;6R", "[1;7R", "[1;8R"}},
    [XKB_KEY_F4] = {
        {"OS", "[1;2S", "[1;3S", "[1;4S", "[1;5S", "[1;6S", "[1;7S", "[1;8S"},
        {"OS", "[1;2S", "[1;3S", "[1;4S", "[1;5S", "[1;6S", "[1;7S", "[1;8S"}},
    [XKB_KEY_F5] = {
        {"[15~", "[15;2~", "[15;3~", "[15;4~", "[15;5~", "[15;6~", "[15;7~", "[15;8~"},
        {"[15~", "[15;2~", "[15;3~", "[15;4~", "[15;5~", "[15;6~", "[15;7~", "[15;8~"}},
    [XKB_KEY_F6] = {
        {"[17~", "[17;2~", "[17;3~", "[17;4~", "[17;5~", "[17;6~", "[17;7~", "[17;8~"},
        {"[17~", "[17;2~", "[17;3~", "[17;4~", "[17;5~", "[17;6~", "[17;7~", "[17;8~"}},
    [XKB_KEY_F7] = {
        {"[18~", "[18;2~", "[18;3~", "[18;4~", "[18;5~", "[18;6~", "[18;7~", "[18;8~"},
        {"[18~", "[18;2~", "[18;3~", "[18;4~", "[18;5~", "[18;6~", "[18;7~", "[18;8~"}},
    [XKB_KEY_F8] = {
        {"[19~", "[19;2~", "[19;3~", "[19;4~", "[19;5~", "[19;6~", "[19;7~", "[19;8~"},
        {"[19~", "[19;2~", "[19;3~", "[19;4~", "[19;5~", "[19;6~", "[19;7~", "[19;8~"}},
    [XKB_KEY_F9] = {
        {"[20~", "[20;2~", "[20;3~", "[20;4~", "[20;5~", "[20;6~", "[20;7~", "[20;8~"},
        {"[20~", "[20;2~", "[20;3~", "[20;4~", "[20;5~", "[20;6~", "[20;7~", "[20;8~"}},
    [XKB_KEY_F10] = {
        {"[21~", "[21;2~", "[21;3~", "[21;4~", "[21;5~", "[21;6~", "[21;7~", "[21;8~"},
        {"[21~", "[21;2~", "[21;3~", "[21;4~", "[21;5~", "[21;6~", "[21;7~", "[21;8~"}},
    [XKB_KEY_F11] = {
        {"[23~", "[23;2~", "[23;3~", "[23;4~", "[23;5~", "[23;6~", "[23;7~", "[23;8~"},
        {"[23~", "[23;2~", "[23;3~", "[23;4~", "[23;5~", "[23;6~", "[23;7~", "[23;8~"}},
    [XKB_KEY_F12] = {
        {"[24~", "[24;2~", "[24;3~", "[24;4~", "[24;5~", "[24;6~", "[24;7~", "[24;8~"},
        {"[24~", "[24;2~", "[24;3~", "[24;4~", "[24;5~", "[24;6~", "[24;7~", "[24;8~"}},
    };

static void
keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
             uint32_t time, uint32_t key, uint32_t state)
{
    static bool mod_masks_initialized = false;
    static xkb_mod_mask_t ctrl = -1;
    static xkb_mod_mask_t alt = -1;
    static xkb_mod_mask_t shift = -1;

    struct terminal *term = data;

    if (!mod_masks_initialized) {
        mod_masks_initialized = true;
        ctrl = 1 << xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Control");
        alt = 1 << xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Mod1");
        shift = 1 << xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Shift");
    }

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

    if (sym < sizeof(key_map) / sizeof(key_map[0]) &&
        key_map[sym][term->decckm].normal != NULL)
    {
        const struct keymap *key = &key_map[sym][term->decckm];
        const char *esc = NULL;

        if (effective_mods == 0)
            esc = key->normal;
        else if (effective_mods == shift)
            esc = key->shift;
        else if (effective_mods == alt)
            esc = key->alt;
        else if (effective_mods == (shift | alt))
            esc = key->shift_alt;
        else if (effective_mods == ctrl)
            esc = key->ctrl;
        else if (effective_mods == (shift | ctrl))
            esc = key->shift_ctrl;
        else if (effective_mods == (alt | ctrl))
            esc = key->alt_ctrl;
        else if (effective_mods == (shift | alt | ctrl))
            esc = key->shift_alt_ctrl;
        else
            assert(false);

        write(term->ptmx, "\x1b", 1);
        write(term->ptmx, esc, strlen(esc));
    } else if (sym == XKB_KEY_Escape){
        write(term->ptmx, "\x1b", 1);
    } else {
        /* TODO: composing */

        char buf[64] = {0};
        int count = xkb_state_key_get_utf8(
            term->kbd.xkb_state, key, buf, sizeof(buf));

        if (count > 0) {
            if (effective_mods & alt)
                write(term->ptmx, "\x1b", 1);

            write(term->ptmx, buf, count);
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
    term->mouse.x = wl_fixed_to_int(surface_x);
    term->mouse.y = wl_fixed_to_int(surface_y);

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
    term->mouse.x = wl_fixed_to_int(surface_x) * 1;//backend->monitor->scale;
    term->mouse.y = wl_fixed_to_int(surface_y) * 1;//backend->monitor->scale;
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
                  uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    LOG_DBG("BUTTON: button=%x, state=%u", button, state);

    struct terminal *term = data;

    static bool mods_initialized = false;
    static xkb_mod_index_t shift, alt, ctrl;

    if (!mods_initialized) {
        struct xkb_keymap *map = term->kbd.xkb_keymap;
        shift = xkb_keymap_mod_get_index(map, "Shift");
        alt = xkb_keymap_mod_get_index(map, "Mod1") ;
        ctrl = xkb_keymap_mod_get_index(map, "Control");
        mods_initialized = true;
    }

    int col = term->mouse.x / term->cell_width;
    int row = term->mouse.y / term->cell_height;

    struct xkb_state *xkb = term->kbd.xkb_state;
    bool shift_active = xkb_state_mod_index_is_active(
        xkb, shift, XKB_STATE_MODS_DEPRESSED);
    bool alt_active = xkb_state_mod_index_is_active(
        xkb, alt, XKB_STATE_MODS_DEPRESSED);
    bool ctrl_active = xkb_state_mod_index_is_active(
        xkb, ctrl, XKB_STATE_MODS_DEPRESSED);

    switch (state) {
    case WL_POINTER_BUTTON_STATE_PRESSED:
        term_mouse_down(term, button, row, col, shift_active, alt_active, ctrl_active);
        break;

    case WL_POINTER_BUTTON_STATE_RELEASED:
        term_mouse_up(term, button, row, col, shift_active, alt_active, ctrl_active);
        break;
    }
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
                uint32_t time, uint32_t axis, wl_fixed_t value)
{
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
