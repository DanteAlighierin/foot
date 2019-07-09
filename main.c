#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <locale.h>
#include <getopt.h>
#include <poll.h>
#include <errno.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-shell.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "font.h"
#include "grid.h"
#include "input.h"
#include "render.h"
#include "shm.h"
#include "slave.h"
#include "terminal.h"
#include "vt.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

static const struct rgb default_foreground = {0.86, 0.86, 0.86};
static const struct rgb default_background = {0.067, 0.067, 0.067};

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
    struct terminal *term = data;
    if (format == WL_SHM_FORMAT_ARGB8888)
        term->wl.have_argb8888 = true;
}

static const struct wl_shm_listener shm_listener = {
    .format = &shm_format,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    LOG_DBG("wm base ping");
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = &xdg_wm_base_ping,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
                         enum wl_seat_capability caps)
{
    struct terminal *term = data;

    if (term->wl.keyboard != NULL) {
        wl_keyboard_release(term->wl.keyboard);
        term->wl.keyboard = NULL;
    }

    if (term->wl.pointer.pointer != NULL) {
        wl_pointer_release(term->wl.pointer.pointer);
        term->wl.pointer.pointer = NULL;
    }

    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        term->wl.keyboard = wl_seat_get_keyboard(wl_seat);
        wl_keyboard_add_listener(term->wl.keyboard, &keyboard_listener, term);
    }

    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        term->wl.pointer.pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(term->wl.pointer.pointer, &pointer_listener, term);
    }
}

static void
seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

static void
handle_global(void *data, struct wl_registry *registry,
              uint32_t name, const char *interface, uint32_t version)
{
    //LOG_DBG("global: %s", interface);
    struct terminal *term = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        term->wl.compositor = wl_registry_bind(
            term->wl.registry, name, &wl_compositor_interface, 4);
    }

    else if (strcmp(interface, wl_shm_interface.name) == 0) {
        term->wl.shm = wl_registry_bind(
            term->wl.registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(term->wl.shm, &shm_listener, term);
        wl_display_roundtrip(term->wl.display);
    }

    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        term->wl.shell = wl_registry_bind(
            term->wl.registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(term->wl.shell, &xdg_wm_base_listener, term);
    }

    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        term->wl.seat = wl_registry_bind(
            term->wl.registry, name, &wl_seat_interface, 4);
        wl_seat_add_listener(term->wl.seat, &seat_listener, term);
        wl_display_roundtrip(term->wl.display);
    }
}

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                       int32_t width, int32_t height, struct wl_array *states)
{
    //struct context *c = data;
    //LOG_DBG("xdg-toplevel: configure: %dx%d", width, height);
    if (width <= 0 || height <= 0)
        return;

    render_resize(data, width, height);
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    struct terminal *term = data;
    LOG_DBG("xdg-toplevel: close");
    term->quit = true;
    wl_display_roundtrip(term->wl.display);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = &xdg_toplevel_configure,
    .close = &xdg_toplevel_close,
};

static void
xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                      uint32_t serial)
{
    //LOG_DBG("xdg-surface: configure");
    xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = &xdg_surface_configure,
};

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    LOG_WARN("global removed: %u", name);
    assert(false);
}

static const struct wl_registry_listener registry_listener = {
    .global = &handle_global,
    .global_remove = &handle_global_remove,
};

static int
keyboard_repeater(void *arg)
{
    struct terminal *term = arg;

    while (true) {
        LOG_DBG("repeater: waiting for start");

        mtx_lock(&term->kbd.repeat.mutex);
        while (term->kbd.repeat.cmd == REPEAT_STOP)
            cnd_wait(&term->kbd.repeat.cond, &term->kbd.repeat.mutex);

        if (term->kbd.repeat.cmd == REPEAT_EXIT) {
            mtx_unlock(&term->kbd.repeat.mutex);
            return 0;
        }

    restart:

        LOG_DBG("repeater: started");
        assert(term->kbd.repeat.cmd == REPEAT_START);

        const long rate_delay = 1000000000 / term->kbd.repeat.rate;
        long delay = term->kbd.repeat.delay * 1000000;

        while (true) {
            assert(term->kbd.repeat.rate > 0);

            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);

            timeout.tv_nsec += delay;
            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec += timeout.tv_nsec / 1000000000;
                timeout.tv_nsec %= 1000000000;
            }

            int ret = cnd_timedwait(&term->kbd.repeat.cond, &term->kbd.repeat.mutex, &timeout);
            if (ret == thrd_success) {
                if (term->kbd.repeat.cmd == REPEAT_START)
                    goto restart;
                else if (term->kbd.repeat.cmd == REPEAT_STOP) {
                    mtx_unlock(&term->kbd.repeat.mutex);
                    break;
                } else if (term->kbd.repeat.cmd == REPEAT_EXIT) {
                    mtx_unlock(&term->kbd.repeat.mutex);
                    return 0;
                }
            }

            assert(ret == thrd_timedout);
            assert(term->kbd.repeat.cmd == REPEAT_START);
            LOG_DBG("repeater: repeat: %u", term->kbd.repeat.key);

            if (write(term->kbd.repeat.pipe_write_fd, &term->kbd.repeat.key,
                      sizeof(term->kbd.repeat.key)) != sizeof(term->kbd.repeat.key))
            {
                LOG_ERRNO("faile to write repeat key to repeat pipe");
                mtx_unlock(&term->kbd.repeat.mutex);
                return 0;
            }

            delay = rate_delay;
        }

    }

    assert(false);
    return 1;
}

int
main(int argc, char *const *argv)
{
    int ret = EXIT_FAILURE;

    static const struct option longopts[] =  {
        {"font", required_argument, 0, 'f'},
        {NULL,   no_argument,       0,   0},
    };

    const char *font_name = "Dina:pixelsize=12";

    while (true) {
        int c = getopt_long(argc, argv, ":f:h", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 'f':
            font_name = optarg;
            break;

        case 'h':
            break;

        case ':':
            fprintf(stderr, "error: -%c: missing required argument\n", optopt);
            return EXIT_FAILURE;

        case '?':
            fprintf(stderr, "error: -%c: invalid option\n", optopt);
            return EXIT_FAILURE;
        }
    }

    setlocale(LC_ALL, "");

    int repeat_pipe_fds[2] = {-1, -1};
    if (pipe2(repeat_pipe_fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe for repeater thread");
        return ret;
    }

    struct terminal term = {
        .quit = false,
        .ptmx = posix_openpt(O_RDWR | O_NOCTTY),
        .cursor_keys_mode = CURSOR_KEYS_NORMAL,
        .keypad_keys_mode = KEYPAD_NUMERICAL,
        .auto_margin = true,
        .vt = {
            .state = 1,  /* STATE_GROUND */
            .attrs = {
                .foreground = default_foreground,
                .background = default_background,
            },
        },
        .kbd = {
            .repeat = {
                .pipe_read_fd = repeat_pipe_fds[0],
                .pipe_write_fd = repeat_pipe_fds[1],
                .cmd = REPEAT_STOP,
            },
        },
        .foreground = default_foreground,
        .background = default_background,
        .normal = {.damage = tll_init(), .scroll_damage = tll_init()},
        .alt = {.damage = tll_init(), .scroll_damage = tll_init()},
        .grid = &term.normal,
    };

    mtx_init(&term.kbd.repeat.mutex, mtx_plain);
    cnd_init(&term.kbd.repeat.cond);

    thrd_t keyboard_repeater_id;
    thrd_create(&keyboard_repeater_id, &keyboard_repeater, &term);

    term.fonts[0] = font_from_name(font_name);
    if (term.fonts[0] == NULL)
        goto out;

    {
        char fname[1024];
        snprintf(fname, sizeof(fname), "%s:style=bold", font_name);
        term.fonts[1] = font_from_name(fname);

        snprintf(fname, sizeof(fname), "%s:style=italic", font_name);
        term.fonts[2] = font_from_name(fname);

        snprintf(fname, sizeof(fname), "%s:style=bold italic", font_name);
        term.fonts[3] = font_from_name(fname);
    }

    cairo_scaled_font_extents(term.fonts[0],  &term.fextents);
    term.cell_width = (int)ceil(term.fextents.max_x_advance);
    term.cell_height = (int)ceil(term.fextents.height);

    LOG_DBG("font: height: %.2f, x-advance: %.2f",
            term.fextents.height, term.fextents.max_x_advance);
    assert(term.fextents.max_y_advance == 0);

    if (term.ptmx == -1) {
        LOG_ERRNO("failed to open pseudo terminal");
        goto out;
    }

    term.wl.display = wl_display_connect(NULL);
    if (term.wl.display == NULL) {
        LOG_ERR("failed to connect to wayland; no compositor running?");
        goto out;
    }

    term.wl.registry = wl_display_get_registry(term.wl.display);
    if (term.wl.registry == NULL) {
        LOG_ERR("failed to get wayland registry");
        goto out;
    }

    wl_registry_add_listener(term.wl.registry, &registry_listener, &term);
    wl_display_roundtrip(term.wl.display);

    if (term.wl.compositor == NULL) {
        LOG_ERR("no compositor");
        goto out;
    }
    if (term.wl.shm == NULL) {
        LOG_ERR("no shared memory buffers interface");
        goto out;
    }
    if (term.wl.shell == NULL) {
        LOG_ERR("no XDG shell interface");
        goto out;
    }
    if (!term.wl.have_argb8888) {
        LOG_ERR("compositor does not support ARGB surfaces");
        goto out;
    }

    /* Cursor */
    term.wl.pointer.surface = wl_compositor_create_surface(term.wl.compositor);
    if (term.wl.pointer.surface == NULL) {
        LOG_ERR("failed to create cursor surface");
        goto out;
    }

    unsigned cursor_size = 24;
    const char *cursor_theme = getenv("XCURSOR_THEME");

    {
        const char *env_cursor_size = getenv("XCURSOR_SIZE");
        if (env_cursor_size != NULL) {
            unsigned size;
            if (sscanf(env_cursor_size, "%u", &size) == 1)
                cursor_size = size;
        }
    }

    LOG_INFO("cursor theme: %s, size: %u", cursor_theme, cursor_size);

    term.wl.pointer.theme = wl_cursor_theme_load(
        cursor_theme, cursor_size * 1 /* backend->monitor->scale */,
        term.wl.shm);
    if (term.wl.pointer.theme == NULL) {
        LOG_ERR("failed to load cursor theme");
        return false;
    }

    term.wl.pointer.cursor = wl_cursor_theme_get_cursor(
        term.wl.pointer.theme, "left_ptr");
    assert(term.wl.pointer.cursor != NULL);
    render_update_cursor_surface(&term);

    term.wl.surface = wl_compositor_create_surface(term.wl.compositor);
    if (term.wl.surface == NULL) {
        LOG_ERR("failed to create wayland surface");
        goto out;
    }

    term.wl.xdg_surface = xdg_wm_base_get_xdg_surface(term.wl.shell, term.wl.surface);
    xdg_surface_add_listener(term.wl.xdg_surface, &xdg_surface_listener, &term);

    term.wl.xdg_toplevel = xdg_surface_get_toplevel(term.wl.xdg_surface);
    xdg_toplevel_add_listener(term.wl.xdg_toplevel, &xdg_toplevel_listener, &term);

    xdg_toplevel_set_app_id(term.wl.xdg_toplevel, "f00ter");
    render_set_title(&term, "f00ter");

    wl_surface_commit(term.wl.surface);
    wl_display_roundtrip(term.wl.display);

    /* TODO: use font metrics to calculate initial size from ROWS x COLS */
    const int default_width = 300;
    const int default_height = 300;
    render_resize(&term, default_width, default_height);

    wl_display_dispatch_pending(term.wl.display);

    term.slave = fork();
    switch (term.slave) {
    case -1:
        LOG_ERRNO("failed to fork");
        goto out;

    case 0:
        /* Child */
        slave_spawn(term.ptmx);
        assert(false);
        break;

    default:
        LOG_DBG("slave has PID %d", term.slave);
        break;
    }

    /* Read logic requires non-blocking mode */
    {
        int fd_flags = fcntl(term.ptmx, F_GETFL);
        if (fd_flags == -1) {
            LOG_ERRNO("failed to set non blocking mode on PTY master");
            goto out;
        }

        if (fcntl(term.ptmx, F_SETFL, fd_flags | O_NONBLOCK) == -1) {
            LOG_ERRNO("failed to set non blocking mode on PTY master");
            goto out;
        }
    }

    int timeout_ms = -1;

    while (true) {
        struct pollfd fds[] = {
            {.fd = wl_display_get_fd(term.wl.display), .events = POLLIN},
            {.fd = term.ptmx,                     .events = POLLIN},
            {.fd = term.kbd.repeat.pipe_read_fd,  .events = POLLIN},
        };

        wl_display_flush(term.wl.display);
        int ret = poll(fds, sizeof(fds) / sizeof(fds[0]), timeout_ms);

        if (ret == -1) {
            if (errno == EINTR)
                continue;

            LOG_ERRNO("failed to poll file descriptors");
            break;
        }

        if (ret == 0 || !(timeout_ms != -1 && fds[1].revents & POLLIN)) {
            /* Delayed rendering */
            if (term.frame_callback == NULL)
                grid_render(&term);
        }

        /* Reset poll timeout to infinity */
        timeout_ms = -1;

        if (fds[0].revents & POLLIN) {
            wl_display_dispatch(term.wl.display);
            if (term.quit) {
                ret = EXIT_SUCCESS;
                break;
            }
        }

        if (fds[0].revents & POLLHUP) {
            LOG_WARN("disconnected from wayland");
            break;
        }

        if (fds[1].revents & POLLIN) {
            uint8_t data[8192];
            ssize_t count = read(term.ptmx, data, sizeof(data));
            if (count < 0) {
                if (errno != EAGAIN)
                    LOG_ERRNO("failed to read from pseudo terminal");
                break;
            }

            vt_from_slave(&term, data, count);

            /*
             * We likely need to re-render. But, we don't want to do
             * it immediately. Often, a single client operation is
             * done through multiple writes. Many times, we're so fast
             * that we render mid-operation frames.
             *
             * For example, we might end up rendering a frame where
             * the client just erased a line, while in the next frame,
             * the client wrote to the same line. This causes screen
             * "flashes".
             *
             * Mitigate by always incuring a small delay before
             * rendering the next frame. This gives the client some
             * time to finish the operation (and thus gives us time to
             * receive the last writes before doing any actual
             * rendering).
             *
             * Note that when the client is producing data at a very
             * high pace, we're rate limited by the wayland compositor
             * anyway. The delay we introduce here only has any effect
             * when the renderer is idle.
             *
             * TODO: this adds input latency. Can we somehow hint
             * ourselves we just received keyboard input, and in this
             * case *not* delay rendering?
             */
            timeout_ms = 1;
        }

        if (fds[1].revents & POLLHUP) {
            ret = EXIT_SUCCESS;
            break;
        }

        if (fds[2].revents & POLLIN) {
            uint32_t key;
            if (read(term.kbd.repeat.pipe_read_fd, &key, sizeof(key)) != sizeof(key)) {
                LOG_ERRNO("failed to read repeat key from repeat pipe");
                break;
            }

            term.kbd.repeat.dont_re_repeat = true;
            input_repeat(&term, key);
            term.kbd.repeat.dont_re_repeat = false;
        }

        if (fds[2].revents & POLLHUP)
            LOG_ERR("keyboard repeat handling thread died");
    }

out:
    mtx_lock(&term.kbd.repeat.mutex);
    term.kbd.repeat.cmd = REPEAT_EXIT;
    cnd_signal(&term.kbd.repeat.cond);
    mtx_unlock(&term.kbd.repeat.mutex);

    shm_fini();
    if (term.frame_callback != NULL)
        wl_callback_destroy(term.frame_callback);
    if (term.wl.xdg_toplevel != NULL)
        xdg_toplevel_destroy(term.wl.xdg_toplevel);
    if (term.wl.xdg_surface != NULL)
        xdg_surface_destroy(term.wl.xdg_surface);
    if (term.wl.pointer.theme != NULL)
        wl_cursor_theme_destroy(term.wl.pointer.theme);
    if (term.wl.pointer.pointer != NULL)
        wl_pointer_destroy(term.wl.pointer.pointer);
    if (term.wl.pointer.surface != NULL)
        wl_surface_destroy(term.wl.pointer.surface);
    if (term.wl.keyboard != NULL)
        wl_keyboard_destroy(term.wl.keyboard);
    if (term.wl.seat != NULL)
        wl_seat_destroy(term.wl.seat);
    if (term.wl.surface != NULL)
        wl_surface_destroy(term.wl.surface);
    if (term.wl.shell != NULL)
        xdg_wm_base_destroy(term.wl.shell);
    if (term.wl.shm != NULL)
        wl_shm_destroy(term.wl.shm);
    if (term.wl.compositor != NULL)
        wl_compositor_destroy(term.wl.compositor);
    if (term.wl.registry != NULL)
        wl_registry_destroy(term.wl.registry);
    if (term.wl.display != NULL)
        wl_display_disconnect(term.wl.display);
    if (term.kbd.xkb_compose_state != NULL)
        xkb_compose_state_unref(term.kbd.xkb_compose_state);
    if (term.kbd.xkb_compose_table != NULL)
        xkb_compose_table_unref(term.kbd.xkb_compose_table);
    if (term.kbd.xkb_keymap != NULL)
        xkb_keymap_unref(term.kbd.xkb_keymap);
    if (term.kbd.xkb_state != NULL)
        xkb_state_unref(term.kbd.xkb_state);
    if (term.kbd.xkb != NULL)
        xkb_context_unref(term.kbd.xkb);

    for (int row = 0; row < term.normal.num_rows; row++) {
        free(term.normal.rows[row]->cells);
        free(term.normal.rows[row]);
    }
    free(term.normal.rows);
    for (int row = 0; row < term.alt.num_rows; row++) {
        free(term.alt.rows[row]->cells);
        free(term.alt.rows[row]);
    }
    free(term.alt.rows);

    for (size_t i = 0; i < sizeof(term.fonts) / sizeof(term.fonts[0]); i++) {
        if (term.fonts[i] != NULL)
            cairo_scaled_font_destroy(term.fonts[i]);
    }

    if (term.ptmx != -1)
        close(term.ptmx);

    thrd_join(keyboard_repeater_id, NULL);
    cnd_destroy(&term.kbd.repeat.cond);
    mtx_destroy(&term.kbd.repeat.mutex);
    close(term.kbd.repeat.pipe_read_fd);
    close(term.kbd.repeat.pipe_write_fd);

    cairo_debug_reset_static_data();
    return ret;
}
