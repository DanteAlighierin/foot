#include "input.h"

#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <threads.h>
#include <locale.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <fcntl.h>

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
#include "macros.h"
#include "quirks.h"
#include "render.h"
#include "search.h"
#include "selection.h"
#include "spawn.h"
#include "terminal.h"
#include "tokenize.h"
#include "util.h"
#include "vt.h"
#include "xmalloc.h"

struct pipe_context {
    char *text;
    size_t idx;
    size_t left;
};

static bool
fdm_write_pipe(struct fdm *fdm, int fd, int events, void *data)
{
    struct pipe_context *ctx = data;

    if (events & EPOLLHUP)
        goto pipe_closed;

    assert(events & EPOLLOUT);
    ssize_t written = write(fd, &ctx->text[ctx->idx], ctx->left);

    if (written < 0) {
        LOG_WARN("failed to write to pipe: %s", strerror(errno));
        goto pipe_closed;
    }

    assert(written <= ctx->left);
    ctx->idx += written;
    ctx->left -= written;

    if (ctx->left == 0)
        goto pipe_closed;

    return true;

pipe_closed:
    free(ctx->text);
    free(ctx);
    fdm_del(fdm, fd);
    return true;
}

static bool
execute_binding(struct seat *seat, struct terminal *term,
                enum bind_action_normal action, char *const *pipe_argv,
                uint32_t serial)
{
    const bool cursor_is_on_grid = seat->mouse.col >= 0 && seat->mouse.row >= 0;

    switch (action) {
    case BIND_ACTION_NONE:
        return true;

    case BIND_ACTION_SCROLLBACK_UP_PAGE:
        cmd_scrollback_up(term, term->rows);
        return true;

    case BIND_ACTION_SCROLLBACK_UP_HALF_PAGE:
        cmd_scrollback_up(term, max(term->rows / 2, 1));
        return true;

    case BIND_ACTION_SCROLLBACK_UP_LINE:
        cmd_scrollback_up(term, 1);
        return true;

    case BIND_ACTION_SCROLLBACK_DOWN_PAGE:
        cmd_scrollback_down(term, term->rows);
        return true;

    case BIND_ACTION_SCROLLBACK_DOWN_HALF_PAGE:
        cmd_scrollback_down(term, max(term->rows / 2, 1));
        return true;

    case BIND_ACTION_SCROLLBACK_DOWN_LINE:
        cmd_scrollback_down(term, 1);
        return true;

    case BIND_ACTION_CLIPBOARD_COPY:
        selection_to_clipboard(seat, term, serial);
        return true;

    case BIND_ACTION_CLIPBOARD_PASTE:
        selection_from_clipboard(seat, term, serial);
        term_reset_view(term);
        return true;

    case BIND_ACTION_PRIMARY_PASTE:
        selection_from_primary(seat, term);
        return true;

    case BIND_ACTION_SEARCH_START:
        search_begin(term);
        return true;

    case BIND_ACTION_FONT_SIZE_UP:
        term_font_size_increase(term);
        return true;

    case BIND_ACTION_FONT_SIZE_DOWN:
        term_font_size_decrease(term);
        return true;

    case BIND_ACTION_FONT_SIZE_RESET:
        term_font_size_reset(term);
        return true;

    case BIND_ACTION_SPAWN_TERMINAL:
        term_spawn_new(term);
        return true;

    case BIND_ACTION_MINIMIZE:
        xdg_toplevel_set_minimized(term->window->xdg_toplevel);
        return true;

    case BIND_ACTION_MAXIMIZE:
        if (term->window->is_fullscreen)
            xdg_toplevel_unset_fullscreen(term->window->xdg_toplevel);
        if (term->window->is_maximized)
            xdg_toplevel_unset_maximized(term->window->xdg_toplevel);
        else
            xdg_toplevel_set_maximized(term->window->xdg_toplevel);
        return true;

    case BIND_ACTION_FULLSCREEN:
        if (term->window->is_fullscreen)
            xdg_toplevel_unset_fullscreen(term->window->xdg_toplevel);
        else
            xdg_toplevel_set_fullscreen(term->window->xdg_toplevel, NULL);
        return true;

    case BIND_ACTION_PIPE_SCROLLBACK:
    case BIND_ACTION_PIPE_VIEW:
    case BIND_ACTION_PIPE_SELECTED: {
        if (pipe_argv == NULL)
            return true;

        struct pipe_context *ctx = NULL;

        int pipe_fd[2] = {-1, -1};
        int stdout_fd = -1;
        int stderr_fd = -1;

        char *text = NULL;
        size_t len = 0;

        if (pipe(pipe_fd) < 0) {
            LOG_ERRNO("failed to create pipe");
            goto pipe_err;
        }

        stdout_fd = open("/dev/null", O_WRONLY);
        stderr_fd = open("/dev/null", O_WRONLY);

        if (stdout_fd < 0 || stderr_fd < 0) {
            LOG_ERRNO("failed to open /dev/null");
            goto pipe_err;
        }

        bool success;
        switch (action) {
        case BIND_ACTION_PIPE_SCROLLBACK:
            success = term_scrollback_to_text(term, &text, &len);
            break;

        case BIND_ACTION_PIPE_VIEW:
            success = term_view_to_text(term, &text, &len);
            break;

        case BIND_ACTION_PIPE_SELECTED:
            text = selection_to_text(term);
            success = text != NULL;
            len = text != NULL ? strlen(text) : 0;
            break;

        default:
            assert(false);
            success = false;
            break;
        }

        if (!success)
            goto pipe_err;

        /* Make write-end non-blocking; required by the FDM */
        {
            int flags = fcntl(pipe_fd[1], F_GETFL);
            if (flags < 0 ||
                fcntl(pipe_fd[1], F_SETFL, flags | O_NONBLOCK) < 0)
            {
                LOG_ERRNO("failed to make write-end of pipe non-blocking");
                goto pipe_err;
            }
        }

        /* Make sure write-end is closed on exec() - or the spawned
         * program may not terminate*/
        {
            int flags = fcntl(pipe_fd[1], F_GETFD);
            if (flags < 0 ||
                fcntl(pipe_fd[1], F_SETFD, flags | FD_CLOEXEC) < 0)
            {
                LOG_ERRNO("failed to set FD_CLOEXEC on writeend of pipe");
                goto pipe_err;
            }
        }

        if (!spawn(term->reaper, NULL, pipe_argv, pipe_fd[0], stdout_fd, stderr_fd))
            goto pipe_err;

        /* Close read end */
        close(pipe_fd[0]);

        ctx = xmalloc(sizeof(*ctx));
        *ctx = (struct pipe_context){
            .text = text,
            .left = len,
        };

        /* Asynchronously write the output to the pipe */
        if (!fdm_add(term->fdm, pipe_fd[1], EPOLLOUT, &fdm_write_pipe, ctx))
            goto pipe_err;

        return true;

        pipe_err:
        if (stdout_fd >= 0)
            close(stdout_fd);
        if (stderr_fd >= 0)
            close(stderr_fd);
        if (pipe_fd[0] >= 0)
            close(pipe_fd[0]);
        if (pipe_fd[1] >= 0)
            close(pipe_fd[1]);
        free(text);
        free(ctx);
        return true;
    }

    case BIND_ACTION_SELECT_BEGIN:
        if (selection_enabled(term, seat) && cursor_is_on_grid) {
            selection_start(
                term, seat->mouse.col, seat->mouse.row, SELECTION_NORMAL);
            return true;
        }
        return false;

    case BIND_ACTION_SELECT_BEGIN_BLOCK:
        if (selection_enabled(term, seat) && cursor_is_on_grid) {
            selection_start(
                term, seat->mouse.col, seat->mouse.row, SELECTION_BLOCK);
            return true;
        }
        return false;

    case BIND_ACTION_SELECT_EXTEND:
        if (selection_enabled(term, seat) && cursor_is_on_grid) {
            selection_extend(
                seat, term, seat->mouse.col, seat->mouse.row, serial);
            return true;
        }
        return false;

    case BIND_ACTION_SELECT_WORD:
        if (selection_enabled(term, seat) && cursor_is_on_grid) {
            selection_mark_word(
                seat, term, seat->mouse.col, seat->mouse.row, false, serial);
            return true;
        }
        return false;

    case BIND_ACTION_SELECT_WORD_WS:
        if (selection_enabled(term, seat) && cursor_is_on_grid) {
            selection_mark_word(
                seat, term, seat->mouse.col, seat->mouse.row, true, serial);
            return true;
        }
        return false;

    case BIND_ACTION_SELECT_ROW:
        if (selection_enabled(term, seat) && cursor_is_on_grid) {
            selection_mark_row(seat, term, seat->mouse.row, serial);
            return true;
        }
        return false;

    case BIND_ACTION_COUNT:
        assert(false);
        return false;
    }

    return false;
}

static xkb_mod_mask_t
conf_modifiers_to_mask(const struct seat *seat,
                       const struct config_key_modifiers *modifiers)
{
    xkb_mod_mask_t mods = 0;
    mods |= modifiers->shift << seat->kbd.mod_shift;
    mods |= modifiers->ctrl << seat->kbd.mod_ctrl;
    mods |= modifiers->alt << seat->kbd.mod_alt;
    mods |= modifiers->meta << seat->kbd.mod_meta;
    return mods;
}

static xkb_keycode_list_t
key_codes_for_xkb_sym(struct xkb_keymap *keymap, xkb_keysym_t sym)
{
    xkb_keycode_list_t key_codes = tll_init();

    /*
     * Find all key codes that map to the lower case
     * version of the symbol.
     *
     * This allows us to match bindings in other layouts
     * too.
     */
    xkb_keysym_t lower_sym = xkb_keysym_to_lower(sym);
    struct xkb_state *state = xkb_state_new(keymap);

    for (xkb_keycode_t code = xkb_keymap_min_keycode(keymap);
         code <= xkb_keymap_max_keycode(keymap);
         code++)
    {
        if (xkb_state_key_get_one_sym(state, code) == lower_sym)
            tll_push_back(key_codes, code);
    }

    xkb_state_unref(state);
    return key_codes;
}

static void
convert_key_binding(struct seat *seat,
                    const struct config_key_binding_normal *conf_binding)
{
    struct key_binding_normal binding = {
        .action = conf_binding->action,
        .bind = {
            .mods = conf_modifiers_to_mask(seat, &conf_binding->modifiers),
            .sym = conf_binding->sym,
            .key_codes = key_codes_for_xkb_sym(
                seat->kbd.xkb_keymap, conf_binding->sym),
        },
        .pipe_argv = conf_binding->pipe.argv,
    };
    tll_push_back(seat->kbd.bindings.key, binding);
}

static void
convert_key_bindings(const struct config *conf, struct seat *seat)
{
    tll_foreach(conf->bindings.key, it)
        convert_key_binding(seat, &it->item);
}

static void
convert_search_binding(struct seat *seat,
                       const struct config_key_binding_search *conf_binding)
{
    struct key_binding_search binding = {
        .action = conf_binding->action,
        .bind = {
            .mods = conf_modifiers_to_mask(seat, &conf_binding->modifiers),
            .sym = conf_binding->sym,
            .key_codes = key_codes_for_xkb_sym(
                seat->kbd.xkb_keymap, conf_binding->sym),
        },
    };
    tll_push_back(seat->kbd.bindings.search, binding);
}

static void
convert_search_bindings(const struct config *conf, struct seat *seat)
{
    tll_foreach(conf->bindings.search, it)
        convert_search_binding(seat, &it->item);
}

static void
convert_mouse_binding(struct seat *seat,
                      const struct config_mouse_binding *conf_binding)
{
    struct mouse_binding binding = {
        .action = conf_binding->action,
        .mods = conf_modifiers_to_mask(seat, &conf_binding->modifiers),
        .button = conf_binding->button,
        .count = conf_binding->count,
        .pipe_argv = conf_binding->pipe.argv,
    };
    tll_push_back(seat->mouse.bindings, binding);
}

static void
convert_mouse_bindings(const struct config *conf, struct seat *seat)
{
    tll_foreach(conf->bindings.mouse, it)
        convert_mouse_binding(seat, &it->item);
}

static void
keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                uint32_t format, int32_t fd, uint32_t size)
{
    LOG_DBG("keyboard_keymap: keyboard=%p (format=%u, size=%u)",
            (void *)wl_keyboard, format, size);

    struct seat *seat = data;
    struct wayland *wayl = seat->wayl;

    /*
     * Free old keymap state
     */

    if (seat->kbd.xkb_compose_state != NULL) {
        xkb_compose_state_unref(seat->kbd.xkb_compose_state);
        seat->kbd.xkb_compose_state = NULL;
    }
    if (seat->kbd.xkb_compose_table != NULL) {
        xkb_compose_table_unref(seat->kbd.xkb_compose_table);
        seat->kbd.xkb_compose_table = NULL;
    }
    if (seat->kbd.xkb_keymap != NULL) {
        xkb_keymap_unref(seat->kbd.xkb_keymap);
        seat->kbd.xkb_keymap = NULL;
    }
    if (seat->kbd.xkb_state != NULL) {
        xkb_state_unref(seat->kbd.xkb_state);
        seat->kbd.xkb_state = NULL;
    }
    if (seat->kbd.xkb != NULL) {
        xkb_context_unref(seat->kbd.xkb);
        seat->kbd.xkb = NULL;
    }

    tll_foreach(seat->kbd.bindings.key, it)
        tll_free(it->item.bind.key_codes);
    tll_free(seat->kbd.bindings.key);

    tll_foreach(seat->kbd.bindings.search, it)
        tll_free(it->item.bind.key_codes);
    tll_free(seat->kbd.bindings.search);

    tll_free(seat->mouse.bindings);

    /* Verify keymap is in a format we understand */
    switch ((enum wl_keyboard_keymap_format)format) {
    case WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP:
        return;

    case WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1:
        break;

    default:
        LOG_WARN("unrecognized keymap format: %u", format);
        return;
    }

    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str == MAP_FAILED) {
        LOG_ERRNO("failed to mmap keyboard keymap");
        close(fd);
        return;
    }

    while (map_str[size - 1] == '\0')
        size--;

    seat->kbd.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    if (seat->kbd.xkb != NULL) {
        seat->kbd.xkb_keymap = xkb_keymap_new_from_buffer(
            seat->kbd.xkb, map_str, size, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);

        /* Compose (dead keys) */
        seat->kbd.xkb_compose_table = xkb_compose_table_new_from_locale(
            seat->kbd.xkb, setlocale(LC_CTYPE, NULL), XKB_COMPOSE_COMPILE_NO_FLAGS);

        if (seat->kbd.xkb_compose_table == NULL) {
            LOG_WARN("failed to instantiate compose table; dead keys will not work");
        } else {
            seat->kbd.xkb_compose_state = xkb_compose_state_new(
                seat->kbd.xkb_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
        }
    }

    if (seat->kbd.xkb_keymap != NULL) {
        seat->kbd.xkb_state = xkb_state_new(seat->kbd.xkb_keymap);

        seat->kbd.mod_shift = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_MOD_NAME_SHIFT);
        seat->kbd.mod_alt = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_MOD_NAME_ALT) ;
        seat->kbd.mod_ctrl = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_MOD_NAME_CTRL);
        seat->kbd.mod_meta = xkb_keymap_mod_get_index(seat->kbd.xkb_keymap, XKB_MOD_NAME_LOGO);

        seat->kbd.key_arrow_up = xkb_keymap_key_by_name(seat->kbd.xkb_keymap, "UP");
        seat->kbd.key_arrow_down = xkb_keymap_key_by_name(seat->kbd.xkb_keymap, "DOWN");
    }

    munmap(map_str, size);
    close(fd);

    convert_key_bindings(wayl->conf, seat);
    convert_search_bindings(wayl->conf, seat);
    convert_mouse_bindings(wayl->conf, seat);
}

static void
keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
               struct wl_surface *surface, struct wl_array *keys)
{
    assert(surface != NULL);

    struct seat *seat = data;
    struct wl_window *win = wl_surface_get_user_data(surface);
    struct terminal *term = win->term;

    LOG_DBG("%s: keyboard_enter: keyboard=%p, serial=%u, surface=%p",
            seat->name, (void *)wl_keyboard, serial, (void *)surface);

    if (seat->kbd.xkb == NULL)
        return;

    term_kbd_focus_in(term);
    seat->kbd_focus = term;
    seat->kbd.serial = serial;
}

static bool
start_repeater(struct seat *seat, uint32_t key)
{
    if (seat->kbd.repeat.dont_re_repeat)
        return true;

    struct itimerspec t = {
        .it_value = {.tv_sec = 0, .tv_nsec = seat->kbd.repeat.delay * 1000000},
        .it_interval = {.tv_sec = 0, .tv_nsec = 1000000000 / seat->kbd.repeat.rate},
    };

    if (t.it_value.tv_nsec >= 1000000000) {
        t.it_value.tv_sec += t.it_value.tv_nsec / 1000000000;
        t.it_value.tv_nsec %= 1000000000;
    }
    if (t.it_interval.tv_nsec >= 1000000000) {
        t.it_interval.tv_sec += t.it_interval.tv_nsec / 1000000000;
        t.it_interval.tv_nsec %= 1000000000;
    }
    if (timerfd_settime(seat->kbd.repeat.fd, 0, &t, NULL) < 0) {
        LOG_ERRNO("%s: failed to arm keyboard repeat timer", seat->name);
        return false;
    }

    seat->kbd.repeat.key = key;
    return true;
}

static bool
stop_repeater(struct seat *seat, uint32_t key)
{
    if (key != -1 && key != seat->kbd.repeat.key)
        return true;

    if (timerfd_settime(seat->kbd.repeat.fd, 0, &(struct itimerspec){{0}}, NULL) < 0) {
        LOG_ERRNO("%s: failed to disarm keyboard repeat timer", seat->name);
        return false;
    }

    return true;
}

static void
keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
               struct wl_surface *surface)
{
    struct seat *seat = data;

    LOG_DBG("keyboard_leave: keyboard=%p, serial=%u, surface=%p",
            (void *)wl_keyboard, serial, (void *)surface);

    if (seat->kbd.xkb == NULL)
        return;

    assert(
        seat->kbd_focus == NULL ||
        surface == NULL ||  /* Seen on Sway 1.2 */
        ((const struct wl_window *)wl_surface_get_user_data(surface))->term == seat->kbd_focus
        );

    struct terminal *old_focused = seat->kbd_focus;
    seat->kbd_focus = NULL;

    stop_repeater(seat, -1);
    seat->kbd.shift = false;
    seat->kbd.alt = false;
    seat->kbd.ctrl = false;
    seat->kbd.meta = false;
    if (seat->kbd.xkb_compose_state != NULL)
        xkb_compose_state_reset(seat->kbd.xkb_compose_state);

    if (old_focused != NULL) {
        seat->pointer.hidden = false;
        term_xcursor_update_for_seat(old_focused, seat);
        term_kbd_focus_out(old_focused);
    } else {
        /*
         * Sway bug - under certain conditions we get a
         * keyboard_leave() (and keyboard_key()) without first having
         * received a keyboard_enter()
         */
        LOG_WARN(
            "compositor sent keyboard_leave event without a keyboard_enter "
            "event: surface=%p", (void *)surface);
    }
}

static const struct key_data *
keymap_data_for_sym(xkb_keysym_t sym, size_t *count)
{
    switch (sym) {
    case XKB_KEY_Escape:       *count = ALEN(key_escape);       return key_escape;
    case XKB_KEY_Return:       *count = ALEN(key_return);       return key_return;
    case XKB_KEY_Tab:          /* FALLTHROUGH */
    case XKB_KEY_ISO_Left_Tab: *count = ALEN(key_tab);          return key_tab;
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
    case XKB_KEY_KP_Decimal:   *count = ALEN(key_kp_decimal);   return key_kp_decimal;
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
keymap_lookup(struct seat *seat, struct terminal *term,
              xkb_keysym_t sym, enum modifier mods)
{
    size_t count;
    const struct key_data *info = keymap_data_for_sym(sym, &count);

    if (info == NULL)
        return NULL;

    const enum cursor_keys cursor_keys_mode = term->cursor_keys_mode;
    const enum keypad_keys keypad_keys_mode
        = term->num_lock_modifier ? KEYPAD_NUMERICAL : term->keypad_keys_mode;

    LOG_DBG("keypad mode: %d, num-lock=%d", keypad_keys_mode, seat->kbd.num);

    for (size_t j = 0; j < count; j++) {
        if (info[j].modifiers != MOD_ANY && info[j].modifiers != mods)
            continue;

        if (info[j].cursor_keys_mode != CURSOR_KEYS_DONTCARE &&
            info[j].cursor_keys_mode != cursor_keys_mode)
            continue;

        if (info[j].keypad_keys_mode != KEYPAD_DONTCARE &&
            info[j].keypad_keys_mode != keypad_keys_mode)
            continue;

        return &info[j];
    }

    return NULL;
}

static void
key_press_release(struct seat *seat, struct terminal *term, uint32_t serial,
                  uint32_t key, uint32_t state)
{
    if (seat->kbd.xkb == NULL ||
        seat->kbd.xkb_keymap == NULL ||
        seat->kbd.xkb_state == NULL)
    {
        return;
    }

    const xkb_mod_mask_t ctrl = 1 << seat->kbd.mod_ctrl;
    const xkb_mod_mask_t alt = 1 << seat->kbd.mod_alt;
    const xkb_mod_mask_t shift = 1 << seat->kbd.mod_shift;
    const xkb_mod_mask_t meta = 1 << seat->kbd.mod_meta;

    if (state == XKB_KEY_UP) {
        stop_repeater(seat, key);
        return;
    }

    bool should_repeat = xkb_keymap_key_repeats(seat->kbd.xkb_keymap, key);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(seat->kbd.xkb_state, key);

    if (state == XKB_KEY_DOWN && term->conf->mouse.hide_when_typing &&
        /* TODO: better way to detect modifiers */
        sym != XKB_KEY_Shift_L && sym != XKB_KEY_Shift_R &&
        sym != XKB_KEY_Control_L && sym != XKB_KEY_Control_R &&
        sym != XKB_KEY_Alt_L && sym != XKB_KEY_Alt_R &&
        sym != XKB_KEY_ISO_Level3_Shift &&
        sym != XKB_KEY_Super_L && sym != XKB_KEY_Super_R &&
        sym != XKB_KEY_Meta_L && sym != XKB_KEY_Meta_R &&
        sym != XKB_KEY_Menu)
    {
        seat->pointer.hidden = true;
        term_xcursor_update_for_seat(term, seat);
    }

    enum xkb_compose_status compose_status = XKB_COMPOSE_NOTHING;

    if (seat->kbd.xkb_compose_state != NULL) {
        xkb_compose_state_feed(seat->kbd.xkb_compose_state, sym);
        compose_status = xkb_compose_state_get_status(
            seat->kbd.xkb_compose_state);
    }

    if (compose_status == XKB_COMPOSE_COMPOSING) {
        /* TODO: goto maybe_repeat? */
        return;
    }

    xkb_mod_mask_t mods = xkb_state_serialize_mods(
        seat->kbd.xkb_state, XKB_STATE_MODS_DEPRESSED);
    //xkb_mod_mask_t consumed = xkb_state_key_get_consumed_mods(seat->kbd.xkb_state, key);
    xkb_mod_mask_t consumed = 0x0;
    xkb_mod_mask_t significant = ctrl | alt | shift | meta;
    xkb_mod_mask_t effective_mods = mods & ~consumed & significant;

    if (term->is_searching) {
        if (should_repeat)
            start_repeater(seat, key);
        search_input(seat, term, key, sym, effective_mods, serial);
        return;
    }

#if 0
    for (size_t i = 0; i < 32; i++) {
        if (mods & (1 << i)) {
            LOG_INFO("%s", xkb_keymap_mod_get_name(seat->kbd.xkb_keymap, i));
        }
    }
#endif

#if defined(_DEBUG) && defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    char sym_name[100];
    xkb_keysym_get_name(sym, sym_name, sizeof(sym_name));
#endif

    LOG_DBG("%s (%u/0x%x): seat=%s, term=%p, serial=%u, "
            "mod=0x%08x, consumed=0x%08x, significant=0x%08x, "
            "effective=0x%08x, repeats=%d",
            sym_name, sym, sym, seat->name, (void *)term, serial,
            mods, consumed, significant, effective_mods, should_repeat);

    /*
     * User configurable bindings
     */
    tll_foreach(seat->kbd.bindings.key, it) {
        if (it->item.bind.mods != effective_mods)
            continue;

        /* Match symbol */
        if (it->item.bind.sym == sym) {
            if (execute_binding(seat, term, it->item.action, it->item.pipe_argv, serial))
                goto maybe_repeat;
        }

        /* Match raw key code */
        tll_foreach(it->item.bind.key_codes, code) {
            if (code->item == key) {
                if (execute_binding(seat, term, it->item.action, it->item.pipe_argv, serial))
                    goto maybe_repeat;
            }
        }
    }

    /*
     * Keys generating escape sequences
     */

    enum modifier keymap_mods = MOD_NONE;
    keymap_mods |= seat->kbd.shift ? MOD_SHIFT : MOD_NONE;
    keymap_mods |= seat->kbd.alt ? MOD_ALT : MOD_NONE;
    keymap_mods |= seat->kbd.ctrl ? MOD_CTRL : MOD_NONE;
    keymap_mods |= seat->kbd.meta ? MOD_META : MOD_NONE;

    const struct key_data *keymap;
    if (sym == XKB_KEY_Escape && keymap_mods == MOD_NONE && term->modify_escape_key) {
        static const struct key_data esc = {.seq = "\033[27;1;27~"};
        keymap = &esc;
    } else
        keymap = keymap_lookup(seat, term, sym, keymap_mods);

    if (keymap != NULL) {
        term_to_slave(term, keymap->seq, strlen(keymap->seq));

        term_reset_view(term);
        selection_cancel(term);
        goto maybe_repeat;
    }

    if (compose_status == XKB_COMPOSE_CANCELLED)
        goto maybe_repeat;

    /*
     * Compose, and maybe emit "normal" character
     */

    assert(seat->kbd.xkb_compose_state != NULL ||
           compose_status != XKB_COMPOSE_COMPOSED);

    int count = compose_status == XKB_COMPOSE_COMPOSED
        ? xkb_compose_state_get_utf8(seat->kbd.xkb_compose_state, NULL, 0)
        : xkb_state_key_get_utf8(seat->kbd.xkb_state, key, NULL, 0);

    if (count <= 0)
        goto maybe_repeat;

    /* Buffer for translated key. Use a static buffer in most cases,
     * and use a malloc:ed buffer when necessary */
    uint8_t buf[32];
    uint8_t *utf8 = count < sizeof(buf) ? buf : xmalloc(count + 1);

    compose_status == XKB_COMPOSE_COMPOSED
        ? xkb_compose_state_get_utf8(
            seat->kbd.xkb_compose_state, (char *)utf8, count + 1)
        : xkb_state_key_get_utf8(
            seat->kbd.xkb_state, key, (char *)utf8, count + 1);

    if (seat->kbd.xkb_compose_state != NULL)
        xkb_compose_state_reset(seat->kbd.xkb_compose_state);

#define is_control_key(x) ((x) >= 0x40 && (x) <= 0x7f)
#define IS_CTRL(x) ((x) < 0x20 || ((x) >= 0x7f && (x) <= 0x9f))

    if ((keymap_mods & MOD_CTRL) &&
        !is_control_key(sym) &&
        (count == 1 && !IS_CTRL(utf8[0])) &&
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
                term_to_slave(term, utf8, count);
            }

            else if (term->meta.eight_bit && count == 1) {
                const wchar_t wc = 0x80 | utf8[0];

                char utf8[8];
                mbstate_t ps = {0};
                size_t chars = wcrtomb(utf8, wc, &ps);

                if (chars != (size_t)-1)
                    term_to_slave(term, utf8, chars);
                else
                    term_to_slave(term, utf8, count);
            }

            else {
                /* Alt ignored */
                term_to_slave(term, utf8, count);
            }
        } else
            term_to_slave(term, utf8, count);
    }

    if (utf8 != buf)
        free(utf8);

    term_reset_view(term);
    selection_cancel(term);

maybe_repeat:
    clock_gettime(
        term->wl->presentation_clock_id, &term->render.input_time);

    if (should_repeat)
        start_repeater(seat, key);
}

static void
keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
             uint32_t time, uint32_t key, uint32_t state)
{
    struct seat *seat = data;
    key_press_release(seat, seat->kbd_focus, serial, key + 8, state);
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
                   uint32_t mods_depressed, uint32_t mods_latched,
                   uint32_t mods_locked, uint32_t group)
{
    struct seat *seat = data;

    LOG_DBG("modifiers: depressed=0x%x, latched=0x%x, locked=0x%x, group=%u",
            mods_depressed, mods_latched, mods_locked, group);

    if (seat->kbd.xkb_state != NULL) {
        xkb_state_update_mask(
            seat->kbd.xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);

        /* Update state of modifiers we're interested in for e.g mouse events */
        seat->kbd.shift = xkb_state_mod_index_is_active(
            seat->kbd.xkb_state, seat->kbd.mod_shift, XKB_STATE_MODS_DEPRESSED);
        seat->kbd.alt = xkb_state_mod_index_is_active(
            seat->kbd.xkb_state, seat->kbd.mod_alt, XKB_STATE_MODS_DEPRESSED);
        seat->kbd.ctrl = xkb_state_mod_index_is_active(
            seat->kbd.xkb_state, seat->kbd.mod_ctrl, XKB_STATE_MODS_DEPRESSED);
        seat->kbd.meta = xkb_state_mod_index_is_active(
            seat->kbd.xkb_state, seat->kbd.mod_meta, XKB_STATE_MODS_DEPRESSED);
    }

    if (seat->kbd_focus && seat->kbd_focus->active_surface == TERM_SURF_GRID)
        term_xcursor_update_for_seat(seat->kbd_focus, seat);
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                     int32_t rate, int32_t delay)
{
    struct seat *seat = data;
    LOG_DBG("keyboard repeat: rate=%d, delay=%d", rate, delay);
    seat->kbd.repeat.rate = rate;
    seat->kbd.repeat.delay = delay;
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
input_repeat(struct seat *seat, uint32_t key)
{
    /* Should be cleared as soon as we loose focus */
    assert(seat->kbd_focus != NULL);
    struct terminal *term = seat->kbd_focus;

    key_press_release(seat, term, seat->kbd.serial, key, XKB_KEY_DOWN);
}

static bool
is_top_left(const struct terminal *term, int x, int y)
{
    int csd_border_size = term->conf->csd.border_width;
    return (
        (!term->window->is_tiled_top && !term->window->is_tiled_left) &&
        ((term->active_surface == TERM_SURF_BORDER_LEFT && y < 10 * term->scale) ||
         (term->active_surface == TERM_SURF_BORDER_TOP && x < (10 + csd_border_size) * term->scale)));
}

static bool
is_top_right(const struct terminal *term, int x, int y)
{
    int csd_border_size = term->conf->csd.border_width;
    return (
        (!term->window->is_tiled_top && !term->window->is_tiled_right) &&
        ((term->active_surface == TERM_SURF_BORDER_RIGHT && y < 10 * term->scale) ||
         (term->active_surface == TERM_SURF_BORDER_TOP && x > term->width + 1 * csd_border_size * term->scale - 10 * term->scale)));
}

static bool
is_bottom_left(const struct terminal *term, int x, int y)
{
    int csd_title_size = term->conf->csd.title_height;
    int csd_border_size = term->conf->csd.border_width;
    return (
        (!term->window->is_tiled_bottom && !term->window->is_tiled_left) &&
        ((term->active_surface == TERM_SURF_BORDER_LEFT && y > csd_title_size * term->scale + term->height) ||
         (term->active_surface == TERM_SURF_BORDER_BOTTOM && x < (10 + csd_border_size) * term->scale)));
}

static bool
is_bottom_right(const struct terminal *term, int x, int y)
{
    int csd_title_size = term->conf->csd.title_height;
    int csd_border_size = term->conf->csd.border_width;
    return (
        (!term->window->is_tiled_bottom && !term->window->is_tiled_right) &&
        ((term->active_surface == TERM_SURF_BORDER_RIGHT && y > csd_title_size * term->scale + term->height) ||
         (term->active_surface == TERM_SURF_BORDER_BOTTOM && x > term->width + 1 * csd_border_size * term->scale - 10 * term->scale)));
}

static const char *
xcursor_for_csd_border(struct terminal *term, int x, int y)
{
    if (is_top_left(term, x, y))                              return XCURSOR_TOP_LEFT_CORNER;
    else if (is_top_right(term, x, y))                        return XCURSOR_TOP_RIGHT_CORNER;
    else if (is_bottom_left(term, x, y))                      return XCURSOR_BOTTOM_LEFT_CORNER;
    else if (is_bottom_right(term, x, y))                     return XCURSOR_BOTTOM_RIGHT_CORNER;
    else if (term->active_surface == TERM_SURF_BORDER_LEFT)   return XCURSOR_LEFT_SIDE;
    else if (term->active_surface == TERM_SURF_BORDER_RIGHT)  return XCURSOR_RIGHT_SIDE;
    else if (term->active_surface == TERM_SURF_BORDER_TOP)    return XCURSOR_TOP_SIDE;
    else if (term->active_surface == TERM_SURF_BORDER_BOTTOM) return XCURSOR_BOTTOM_SIDE;
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

    struct seat *seat = data;
    struct wl_window *win = wl_surface_get_user_data(surface);
    struct terminal *term = win->term;

    seat->pointer.serial = serial;
    seat->pointer.hidden = false;

    LOG_DBG("pointer-enter: pointer=%p, serial=%u, surface = %p, new-moused = %p",
            (void *)wl_pointer, serial, (void *)surface, (void *)term);

    /* Scale may have changed */
    wayl_reload_xcursor_theme(seat, term->scale);

    seat->mouse_focus = term;

    int x = wl_fixed_to_int(surface_x) * term->scale;
    int y = wl_fixed_to_int(surface_y) * term->scale;

    switch ((term->active_surface = term_surface_kind(term, surface))) {
    case TERM_SURF_GRID: {
        /*
         * Translate x,y pixel coordinate to a cell coordinate, or -1
         * if the cursor is outside the grid. I.e. if it is inside the
         * margins.
         */

        if (x < term->margins.left || x >= term->width - term->margins.right)
            seat->mouse.col = -1;
        else
            seat->mouse.col = (x - term->margins.left) / term->cell_width;

        if (y < term->margins.top || y >= term->height - term->margins.bottom)
            seat->mouse.row = -1;
        else
            seat->mouse.row = (y - term->margins.top) / term->cell_height;

        term_xcursor_update_for_seat(term, seat);
        break;
    }

    case TERM_SURF_SEARCH:
    case TERM_SURF_SCROLLBACK_INDICATOR:
    case TERM_SURF_RENDER_TIMER:
    case TERM_SURF_TITLE:
        render_xcursor_set(seat, term, XCURSOR_LEFT_PTR);
        break;

    case TERM_SURF_BORDER_LEFT:
    case TERM_SURF_BORDER_RIGHT:
    case TERM_SURF_BORDER_TOP:
    case TERM_SURF_BORDER_BOTTOM:
        render_xcursor_set(seat, term, xcursor_for_csd_border(term, x, y));
        break;

    case TERM_SURF_BUTTON_MINIMIZE:
    case TERM_SURF_BUTTON_MAXIMIZE:
    case TERM_SURF_BUTTON_CLOSE:
        render_xcursor_set(seat, term, XCURSOR_LEFT_PTR);
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
    struct seat *seat = data;
    struct terminal *old_moused = seat->mouse_focus;

    LOG_DBG(
        "%s: pointer-leave: pointer=%p, serial=%u, surface = %p, old-moused = %p",
        seat->name, (void *)wl_pointer, serial, (void *)surface,
        (void *)old_moused);

    seat->pointer.hidden = false;

    if (seat->pointer.xcursor_callback != NULL) {
        /* A cursor frame callback may never be called if the pointer leaves our surface */
        wl_callback_destroy(seat->pointer.xcursor_callback);
        seat->pointer.xcursor_callback = NULL;
        seat->pointer.xcursor_pending = false;
        seat->pointer.xcursor = NULL;
    }

    /* Reset mouse state */
    seat->mouse.x = seat->mouse.y = 0;
    seat->mouse.col = seat->mouse.row = 0;
    seat->mouse.button = seat->mouse.last_button = seat->mouse.button_for_motion_events = seat->mouse.count = 0;
    seat->mouse.consumed = false;
    memset(&seat->mouse.last_time, 0, sizeof(seat->mouse.last_time));
    seat->mouse.axis_aggregated = 0.0;
    seat->mouse.have_discrete = false;

    seat->mouse_focus = NULL;
    if (old_moused == NULL) {
        LOG_WARN(
            "compositor sent pointer_leave event without a pointer_enter "
            "event: surface=%p", (void *)surface);
    } else {
        if (surface != NULL) {
            /* Sway 1.4 sends this event with a NULL surface when we destroy the window */
            const struct wl_window UNUSED *win = wl_surface_get_user_data(surface);
            assert(old_moused == win->term);
        }

        enum term_surface active_surface = old_moused->active_surface;

        old_moused->active_surface = TERM_SURF_NONE;
        term_xcursor_update_for_seat(old_moused, seat);

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
        case TERM_SURF_SCROLLBACK_INDICATOR:
        case TERM_SURF_RENDER_TIMER:
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
    struct seat *seat = data;
    struct wayland *wayl = seat->wayl;
    struct terminal *term = seat->mouse_focus;
    struct wl_window *win = term->window;

    LOG_DBG("pointer_motion: pointer=%p, x=%d, y=%d", (void *)wl_pointer,
            wl_fixed_to_int(surface_x), wl_fixed_to_int(surface_y));

    assert(term != NULL);

    int x = wl_fixed_to_int(surface_x) * term->scale;
    int y = wl_fixed_to_int(surface_y) * term->scale;

    seat->pointer.hidden = false;
    seat->mouse.x = x;
    seat->mouse.y = y;

    switch (term->active_surface) {
    case TERM_SURF_NONE:
    case TERM_SURF_SEARCH:
    case TERM_SURF_SCROLLBACK_INDICATOR:
    case TERM_SURF_RENDER_TIMER:
    case TERM_SURF_BUTTON_MINIMIZE:
    case TERM_SURF_BUTTON_MAXIMIZE:
    case TERM_SURF_BUTTON_CLOSE:
        break;

    case TERM_SURF_TITLE:
        /* We've started a 'move' timer, but user started dragging
         * right away - abort the timer and initiate the actual move
         * right away */
        if (seat->mouse.button == BTN_LEFT && win->csd.move_timeout_fd != -1) {
            fdm_del(wayl->fdm, win->csd.move_timeout_fd);
            win->csd.move_timeout_fd = -1;
            xdg_toplevel_move(win->xdg_toplevel, seat->wl_seat, win->csd.serial);
        }
        break;

    case TERM_SURF_BORDER_LEFT:
    case TERM_SURF_BORDER_RIGHT:
    case TERM_SURF_BORDER_TOP:
    case TERM_SURF_BORDER_BOTTOM:
        render_xcursor_set(seat, term, xcursor_for_csd_border(term, x, y));
        break;

    case TERM_SURF_GRID: {
        int old_col = seat->mouse.col;
        int old_row = seat->mouse.row;

        /*
         * While the seat's mouse coordinates must always be on the
         * grid, or -1, we allow updating the selection even when the
         * mouse is outside the grid (could also be outside the
         * terminal window).
         */
        int selection_col;
        int selection_row;

        if (x < term->margins.left) {
            seat->mouse.col = -1;
            selection_col = 0;
        } else if (x >= term->width - term->margins.right) {
            seat->mouse.col = -1;
            selection_col = term->cols - 1;
        } else {
            seat->mouse.col = (x - term->margins.left) / term->cell_width;
            selection_col = seat->mouse.col;
        }

        if (y < term->margins.top) {
            seat->mouse.row = -1;
            selection_row = 0;
        } else if (y >= term->height - term->margins.bottom) {
            seat->mouse.row = -1;
            selection_row = term->rows - 1;
        } else {
            seat->mouse.row = (y - term->margins.top) / term->cell_height;
            selection_row = seat->mouse.row;
        }

        /*
         * If client is receiving events (because the button was
         * pressed while the cursor was inside the grid area), then
         * make sure it receives valid coordinates.
         */
        if (seat->mouse.button_for_motion_events > 0) {
            seat->mouse.col = selection_col;
            seat->mouse.row = selection_row;
        }

        assert(seat->mouse.col == -1 || (seat->mouse.col >= 0 && seat->mouse.col < term->cols));
        assert(seat->mouse.row == -1 || (seat->mouse.row >= 0 && seat->mouse.row < term->rows));

        term_xcursor_update_for_seat(term, seat);

        /* Cursor has moved to a different cell since last time */
        bool cursor_is_on_new_cell
            = old_col != seat->mouse.col || old_row != seat->mouse.row;

        /* Cursor is inside the grid, i.e. *not* in the margins */
        const bool cursor_is_on_grid = seat->mouse.col >= 0 && seat->mouse.row >= 0;

        enum selection_scroll_direction auto_scroll_direction
            = y < term->margins.top ? SELECTION_SCROLL_UP
            : y > term->height - term->margins.bottom ? SELECTION_SCROLL_DOWN
            : SELECTION_SCROLL_NOT;

        if (auto_scroll_direction == SELECTION_SCROLL_NOT)
            selection_stop_scroll_timer(term);

        /* Update selection */
        if (!term->is_searching) {
            if (auto_scroll_direction != SELECTION_SCROLL_NOT) {
                /*
                 * Start ‘selection auto-scrolling’
                 *
                 * The speed of the scrolling is proportional to the
                 * distance between the mouse and the grid; the
                 * further away the mouse is, the faster we scroll.
                 *
                 * Note that the speed is measured in ‘intervals (in
                 * ns) between each timed scroll of a single line’.
                 *
                 * Thus, the further away the mouse is, the smaller
                 * interval value we use.
                 */

                int distance = auto_scroll_direction == SELECTION_SCROLL_UP
                    ? term->margins.top - y
                    : y - (term->height - term->margins.bottom);

                assert(distance > 0);
                int divisor
                    = distance * term->conf->scrollback.multiplier / term->scale;

                selection_start_scroll_timer(
                    term, 400000000 / (divisor > 0 ? divisor : 1),
                    auto_scroll_direction, selection_col);
            }

            if (cursor_is_on_new_cell || term->selection.end.row < 0)
                selection_update(term, selection_col, selection_row);
        }

        /* Send mouse event to client application */
        if (!seat->mouse.consumed &&
            !term_mouse_grabbed(term, seat) &&
            cursor_is_on_new_cell &&
            (seat->mouse.button_for_motion_events > 0 ||
             (seat->mouse.button == 0 && cursor_is_on_grid)))
        {
            assert(seat->mouse.col < term->cols);
            assert(seat->mouse.row < term->rows);

            term_mouse_motion(
                term,
                (seat->mouse.button_for_motion_events > 0
                 ? seat->mouse.button_for_motion_events : seat->mouse.button),
                seat->mouse.row, seat->mouse.col,
                seat->kbd.shift, seat->kbd.alt, seat->kbd.ctrl);
        }
        break;
    }
    }
}

static bool
fdm_csd_move(struct fdm *fdm, int fd, int events, void *data)
{
    struct seat *seat = data;
    fdm_del(fdm, fd);

    if (seat->mouse_focus == NULL) {
        LOG_WARN(
            "%s: CSD move timeout triggered, but seat's has no mouse focused terminal",
            seat->name);
        return true;
    }

    struct wl_window *win = seat->mouse_focus->window;

    win->csd.move_timeout_fd = -1;
    xdg_toplevel_move(win->xdg_toplevel, seat->wl_seat, win->csd.serial);
    return true;
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
                  uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    LOG_DBG("BUTTON: pointer=%p, serial=%u, button=%x, state=%u",
            (void *)wl_pointer, serial, button, state);

    struct seat *seat = data;
    struct wayland *wayl = seat->wayl;
    struct terminal *term = seat->mouse_focus;

    seat->pointer.hidden = false;

    assert(term != NULL);

    /* Update double/triple click state */
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        /* Time since last click */
        struct timeval now, since_last;
        gettimeofday(&now, NULL);
        timersub(&now, &seat->mouse.last_time, &since_last);

        /* Double- or triple click? */
        if (button == seat->mouse.last_button &&
            since_last.tv_sec == 0 &&
            since_last.tv_usec <= 300 * 1000)
        {
            seat->mouse.count++;
        } else
            seat->mouse.count = 1;

        seat->mouse.button = button; /* For motion events */
        seat->mouse.last_button = button;
        seat->mouse.last_time = now;
    } else
        seat->mouse.button = 0; /* For motion events */

    switch (term->active_surface) {
    case TERM_SURF_TITLE:
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {

            struct wl_window *win = term->window;

            /* Toggle maximized state on double-click */
            if (button == BTN_LEFT && seat->mouse.count == 2) {
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
                    fdm_add(wayl->fdm, fd, EPOLLIN, &fdm_csd_move, seat))
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

            int x = seat->mouse.x;
            int y = seat->mouse.y;

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
                term->window->xdg_toplevel, seat->wl_seat, serial, resize_type);
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
    case TERM_SURF_SCROLLBACK_INDICATOR:
    case TERM_SURF_RENDER_TIMER:
        break;

    case TERM_SURF_GRID: {
        search_cancel(term);

        bool cursor_is_on_grid = seat->mouse.col >= 0 && seat->mouse.row >= 0;

        switch (state) {
        case WL_POINTER_BUTTON_STATE_PRESSED: {
            if (!seat->mouse.consumed) {
                if (seat->wl_keyboard != NULL && seat->kbd.xkb_state != NULL) {
                    /* Seat has keyboard - use mouse bindings *with* modifiers */

                    xkb_mod_mask_t mods = xkb_state_serialize_mods(
                        seat->kbd.xkb_state, XKB_STATE_MODS_DEPRESSED);

                    /* Ignore Shift when matching modifiers, since it is
                     * used to enable selection in mouse grabbing client
                     * applications */
                    mods &= ~(1 << seat->kbd.mod_shift);

                    const struct mouse_binding *match = NULL;

                    tll_foreach(seat->mouse.bindings, it) {
                        const struct mouse_binding *binding = &it->item;

                        if (binding->button != button) {
                            /* Wrong button */
                            continue;
                        }

                        if (binding->mods != mods) {
                            /* Modifier mismatch */
                            continue;
                        }

                        if  (binding->count > seat->mouse.count) {
                            /* Not correct click count */
                            continue;
                        }

                        if (match == NULL || binding->count > match->count)
                            match = binding;
                    }

                    if (match != NULL) {
                        seat->mouse.consumed = execute_binding(
                            seat, term, match->action, match->pipe_argv, serial);
                    }
                }

                else {
                    /* Seat does NOT have a keyboard - use mouse bindings *without* modifiers */
                    const struct config_mouse_binding *match = NULL;

                    tll_foreach(seat->wayl->conf->bindings.mouse, it) {
                        const struct config_mouse_binding *binding = &it->item;

                        if (binding->button != button) {
                            /* Wrong button */
                            continue;
                        }

                        if (binding->count > seat->mouse.count) {
                            /* Incorrect click count */
                            continue;
                        }

                        const struct config_key_modifiers no_mods = {0};
                        if (memcmp(&binding->modifiers, &no_mods, sizeof(no_mods)) != 0) {
                            /* Binding has modifiers */
                            continue;
                        }

                        if (match == NULL || binding->count > match->count)
                            match = binding;
                    }

                    if (match != NULL) {
                        seat->mouse.consumed = execute_binding(
                            seat, term, match->action, match->pipe.argv, serial);
                    }
                }

            }

            if (!seat->mouse.consumed &&
                !term_mouse_grabbed(term, seat) &&
                cursor_is_on_grid)
            {
                term_mouse_down(
                    term, button, seat->mouse.row, seat->mouse.col,
                    seat->kbd.shift, seat->kbd.alt, seat->kbd.ctrl);

                if (seat->mouse.button_for_motion_events == 0)
                    seat->mouse.button_for_motion_events = button;
            }
            break;
        }

        case WL_POINTER_BUTTON_STATE_RELEASED:
            selection_finalize(seat, term, serial);

            if (!seat->mouse.consumed &&
                !term_mouse_grabbed(term, seat) &&
                ((cursor_is_on_grid && seat->mouse.button_for_motion_events > 0) ||
                 seat->mouse.button_for_motion_events == button))
            {
                term_mouse_up(
                    term, button, seat->mouse.row, seat->mouse.col,
                    seat->kbd.shift, seat->kbd.alt, seat->kbd.ctrl);

                if (seat->mouse.button_for_motion_events == button)
                    seat->mouse.button_for_motion_events = 0;
            }

            seat->mouse.consumed = false;
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
alternate_scroll(struct seat *seat, int amount, int button)
{
    if (seat->wl_keyboard == NULL)
        return;

    /* Should be cleared in leave event */
    assert(seat->mouse_focus != NULL);
    struct terminal *term = seat->mouse_focus;

    xkb_keycode_t key = button == BTN_BACK
        ? seat->kbd.key_arrow_up : seat->kbd.key_arrow_down;

    for (int i = 0; i < amount; i++)
        key_press_release(seat, term, seat->kbd.serial, key, XKB_KEY_DOWN);
    key_press_release(seat, term, seat->kbd.serial, key, XKB_KEY_UP);
}

static void
mouse_scroll(struct seat *seat, int amount)
{
    struct terminal *term = seat->mouse_focus;
    assert(term != NULL);

    int button = amount < 0 ? BTN_BACK : BTN_FORWARD;
    amount = abs(amount);

    if (term->mouse_tracking == MOUSE_NONE) {
        if (term->grid == &term->alt) {
            if (term->alt_scrolling)
                alternate_scroll(seat, amount, button);
        } else {
            if (button == BTN_BACK)
                cmd_scrollback_up(term, amount);
            else
                cmd_scrollback_down(term, amount);
        }
    } else if (!term_mouse_grabbed(term, seat) &&
               seat->mouse.col >= 0 && seat->mouse.row >= 0)
    {
        assert(seat->mouse.col < term->cols);
        assert(seat->mouse.row < term->rows);

        for (int i = 0; i < amount; i++) {
            term_mouse_down(
                term, button, seat->mouse.row, seat->mouse.col,
                seat->kbd.shift, seat->kbd.alt, seat->kbd.ctrl);
        }

        term_mouse_up(
            term, button, seat->mouse.row, seat->mouse.col,
            seat->kbd.shift, seat->kbd.alt, seat->kbd.ctrl);
    }
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
                uint32_t time, uint32_t axis, wl_fixed_t value)
{
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    struct seat *seat = data;

    if (seat->mouse.have_discrete)
        return;

    assert(seat->mouse_focus != NULL);

    /*
     * Aggregate scrolled amount until we get at least 1.0
     *
     * Without this, very slow scrolling will never actually scroll
     * anything.
     */
    seat->mouse.axis_aggregated
        += seat->wayl->conf->scrollback.multiplier * wl_fixed_to_double(value);

    if (fabs(seat->mouse.axis_aggregated) < seat->mouse_focus->cell_height)
        return;

    int lines = seat->mouse.axis_aggregated / seat->mouse_focus->cell_height;
    mouse_scroll(seat, lines);
    seat->mouse.axis_aggregated -= (double)lines * seat->mouse_focus->cell_height;
}

static void
wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                         uint32_t axis, int32_t discrete)
{
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    struct seat *seat = data;
    seat->mouse.have_discrete = true;
    mouse_scroll(seat, seat->wayl->conf->scrollback.multiplier * discrete);
}

static void
wl_pointer_frame(void *data, struct wl_pointer *wl_pointer)
{
    struct seat *seat = data;
    seat->mouse.have_discrete = false;
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
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    struct seat *seat = data;
    seat->mouse.axis_aggregated = 0.;
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
