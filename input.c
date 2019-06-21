#include "input.h"

#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <threads.h>
#include <sys/mman.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define LOG_MODULE "input"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "terminal.h"

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

static void
keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
             uint32_t time, uint32_t key, uint32_t state)
{
    static bool mod_masks_initialized = false;
    static xkb_mod_mask_t ctrl = -1;
    static xkb_mod_mask_t alt = -1;
    //static xkb_mod_mask_t shift = -1;

    struct terminal *term = data;

    if (!mod_masks_initialized) {
        mod_masks_initialized = true;
        ctrl = 1 << xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Control");
        alt = 1 << xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Mod1");
        //shift = 1 << xkb_keymap_mod_get_index(term->kbd.xkb_keymap, "Shift");
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
        term->kbd.xkb_state, XKB_STATE_MODS_EFFECTIVE);
    xkb_mod_mask_t consumed = xkb_state_key_get_consumed_mods(term->kbd.xkb_state, key);
    xkb_mod_mask_t significant = ctrl | alt /*| shift*/;
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

    if (sym == XKB_KEY_c && effective_mods == ctrl) {
        kill(term->slave, SIGINT);

    } else if (effective_mods == 0) {
        char buf[128] = {0};
        int count = xkb_state_key_get_utf8(term->kbd.xkb_state, key, buf, sizeof(buf));

        if (count == 0)
            return;

        write(term->ptmx, buf, count);
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
