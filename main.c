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

#include <sys/timerfd.h>

#include <freetype/tttables.h>
#include <cairo-ft.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-shell.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "config.h"
#include "font.h"
#include "grid.h"
#include "input.h"
#include "render.h"
#include "selection.h"
#include "shm.h"
#include "slave.h"
#include "terminal.h"
#include "tokenize.h"
#include "vt.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

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

    else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        term->wl.data_device_manager = wl_registry_bind(
            term->wl.registry, name, &wl_data_device_manager_interface, 1);
    }

    else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        term->wl.primary_selection_device_manager = wl_registry_bind(
            term->wl.registry, name, &zwp_primary_selection_device_manager_v1_interface, 1);
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

    struct config conf = {NULL};
    if (!config_load(&conf))
        return ret;

    static const struct option longopts[] =  {
        {"term", required_argument, 0, 't'},
        {"font", required_argument, 0, 'f'},
        {NULL,   no_argument,       0,   0},
    };

    //const char *font_name = "monospace";

    while (true) {
        int c = getopt_long(argc, argv, ":t:f:h", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 't':
            free(conf.term);
            conf.term = strdup(optarg);
            break;

        case 'f':
            free(conf.font);
            conf.font = strdup(optarg);
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

    argc -= optind;
    argv += optind;

    setlocale(LC_ALL, "");
    setenv("TERM", conf.term, 1);

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
        .window_title_stack = tll_init(),
        .flash = {
            .fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC),
        },
        .blink = {
            .fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC),
        },
        .vt = {
            .state = 1,  /* STATE_GROUND */
            .attrs = {
                //.foreground = conf.colors.fg,
                //.background = conf.colors.bg
            },
        },
        .kbd = {
            .repeat = {
                .pipe_read_fd = repeat_pipe_fds[0],
                .pipe_write_fd = repeat_pipe_fds[1],
                .cmd = REPEAT_STOP,
            },
        },
        .colors = {
            .default_fg = conf.colors.fg,
            .default_bg = conf.colors.bg,
            .default_regular = {
                conf.colors.regular[0],
                conf.colors.regular[1],
                conf.colors.regular[2],
                conf.colors.regular[3],
                conf.colors.regular[4],
                conf.colors.regular[5],
                conf.colors.regular[6],
                conf.colors.regular[7],
            },
            .default_bright = {
                conf.colors.bright[0],
                conf.colors.bright[1],
                conf.colors.bright[2],
                conf.colors.bright[3],
                conf.colors.bright[4],
                conf.colors.bright[5],
                conf.colors.bright[6],
                conf.colors.bright[7],
            },
        },
        .cursor_style = conf.cursor.style,
        .selection = {
            .start = {-1, -1},
            .end = {-1, -1},
        },
        .normal = {.damage = tll_init(), .scroll_damage = tll_init()},
        .alt = {.damage = tll_init(), .scroll_damage = tll_init()},
        .grid = &term.normal,
    };

    /* Initialize 'current' colors from the default colors */
    term.colors.fg = term.colors.default_fg;
    term.colors.bg = term.colors.default_bg;
    for (size_t i = 0; i < 8; i++) {
        term.colors.regular[i] = term.colors.default_regular[i];
        term.colors.bright[i] = term.colors.default_bright[i];
    }

    if (term.ptmx == -1) {
        LOG_ERRNO("failed to open pseudo terminal");
        goto out;
    }

    mtx_init(&term.kbd.repeat.mutex, mtx_plain);
    cnd_init(&term.kbd.repeat.cond);

    thrd_t keyboard_repeater_id;
    thrd_create(&keyboard_repeater_id, &keyboard_repeater, &term);

    term.fonts[0].font = font_from_name(conf.font);
    if (term.fonts[0].font == NULL)
        goto out;

    {
        char fname[1024];
        snprintf(fname, sizeof(fname), "%s:style=bold", conf.font);
        term.fonts[1].font = font_from_name(fname);

        snprintf(fname, sizeof(fname), "%s:style=italic", conf.font);
        term.fonts[2].font = font_from_name(fname);

        snprintf(fname, sizeof(fname), "%s:style=bold italic", conf.font);
        term.fonts[3].font = font_from_name(fname);
    }

    /* Underline position and size */
    for (size_t i = 0; i < sizeof(term.fonts) / sizeof(term.fonts[0]); i++) {
        struct font *f = &term.fonts[i];

        if (f->font == NULL)
            continue;

        FT_Face ft_face = cairo_ft_scaled_font_lock_face(f->font);

        double x_scale = ft_face->size->metrics.x_scale / 65526.;
        double height = ft_face->size->metrics.height / 64;
        double descent = ft_face->size->metrics.descender / 64;

        LOG_DBG("ft: x-scale: %f, height: %f, descent: %f",
                x_scale, height, descent);

        f->underline.position = ft_face->underline_position * x_scale / 64.;
        f->underline.thickness = ft_face->underline_thickness * x_scale / 64.;

        if (f->underline.position == 0.) {
            f->underline.position =  descent / 2.;
            f->underline.thickness =  fabs(round(descent / 5.));
        }

        LOG_DBG("underline: pos=%f, thick=%f",
                f->underline.position, f->underline.thickness);

        TT_OS2 *os2 = FT_Get_Sfnt_Table(ft_face, ft_sfnt_os2);
        if (os2 != NULL) {
            f->strikeout.position = os2->yStrikeoutPosition * x_scale / 64.;
            f->strikeout.thickness = os2->yStrikeoutSize * x_scale / 64.;
        }

        if (f->strikeout.position == 0.) {
            f->strikeout.position = height / 2. + descent;
            f->strikeout.thickness = f->underline.thickness;
        }

        LOG_DBG("strikeout: pos=%f, thick=%f",
                f->strikeout.position, f->strikeout.thickness);

        cairo_ft_scaled_font_unlock_face(f->font);
    }

    cairo_scaled_font_extents(term.fonts[0].font,  &term.fextents);
    term.cell_width = (int)ceil(term.fextents.max_x_advance);
    term.cell_height = (int)ceil(term.fextents.height);

    LOG_DBG("font: height: %.2f, x-advance: %.2f",
            term.fextents.height, term.fextents.max_x_advance);
    assert(term.fextents.max_y_advance == 0);

    /* Glyph cache */
    for (size_t i = 0; i < sizeof(term.fonts) / sizeof(term.fonts[0]); i++) {
        struct font *f = &term.fonts[i];

        for (int j = 0; j < 256; j++) {
            cairo_glyph_t *glyphs = NULL;
            int count = 0;

            char c = j;
            cairo_status_t status = cairo_scaled_font_text_to_glyphs(
                f->font, 0, 0 + term.fextents.ascent,
                &c, 1, &glyphs, &count,
                NULL, NULL, NULL);

            if (status != CAIRO_STATUS_SUCCESS)
                continue;

            if (count == 0)
                continue;

            assert(glyphs != NULL);
            assert(count == 1);

            f->glyph_cache[j].glyphs = glyphs;
            f->glyph_cache[j].count = count;
        }
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
    if (term.wl.seat == NULL) {
        LOG_ERR("no seat available");
        goto out;
    }
    if (term.wl.data_device_manager == NULL) {
        LOG_ERR("no clipboard available "
                "(wl_data_device_manager not implemented by server)");
        goto out;
    }
    if (term.wl.primary_selection_device_manager == NULL) {
        LOG_ERR("no primary selection available");
        goto out;
    }

    /* Clipboard */
    term.wl.data_device = wl_data_device_manager_get_data_device(
        term.wl.data_device_manager, term.wl.seat);
    wl_data_device_add_listener(term.wl.data_device, &data_device_listener, &term);

    /* Primary selection */
    term.wl.primary_selection_device = zwp_primary_selection_device_manager_v1_get_device(
        term.wl.primary_selection_device_manager, term.wl.seat);
    zwp_primary_selection_device_v1_add_listener(
        term.wl.primary_selection_device, &primary_selection_device_listener, &term);

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
        goto out;
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

    xdg_toplevel_set_app_id(term.wl.xdg_toplevel, "foot");
    term_set_window_title(&term, "foot");

    wl_surface_commit(term.wl.surface);
    wl_display_roundtrip(term.wl.display);

    /* TODO: use font metrics to calculate initial size from ROWS x COLS */
    const int default_width = 300;
    const int default_height = 300;
    render_resize(&term, default_width, default_height);

    wl_display_dispatch_pending(term.wl.display);

    {
        int fork_pipe[2];
        if (pipe2(fork_pipe, O_CLOEXEC) < 0) {
            LOG_ERRNO("failed to create pipe");
            goto out;
        }

        term.slave = fork();
        switch (term.slave) {
        case -1:
            LOG_ERRNO("failed to fork");
            close(fork_pipe[0]);
            close(fork_pipe[1]);
            goto out;

        case 0:
            /* Child */
            close(fork_pipe[0]);  /* Close read end */

            char **_shell_argv = NULL;
            char *const *shell_argv = argv;

            if (argc == 0) {
                if (!tokenize_cmdline(conf.shell, &_shell_argv)) {
                    (void)!write(fork_pipe[1], &errno, sizeof(errno));
                    _exit(0);
                }
                shell_argv = _shell_argv;
            }

            slave_spawn(term.ptmx, shell_argv, fork_pipe[1]);
            assert(false);
            break;

        default: {
            close(fork_pipe[1]); /* Close write end */
            LOG_DBG("slave has PID %d", term.slave);

            int _errno;
            static_assert(sizeof(errno) == sizeof(_errno), "errno size mismatch");

            ssize_t ret = read(fork_pipe[0], &_errno, sizeof(_errno));
            close(fork_pipe[0]);

            if (ret < 0) {
                LOG_ERRNO("failed to read from pipe");
                goto out;
            } else if (ret == sizeof(_errno)) {
                LOG_ERRNO(
                    "%s: failed to execute", argc == 0 ? conf.shell : argv[0]);
                goto out;
            } else
                LOG_DBG("%s: successfully started", conf.shell);
            break;
        }
        }
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
            {.fd = term.flash.fd,                 .events = POLLIN},
            {.fd = term.blink.fd,                 .events = POLLIN},
        };

        wl_display_flush(term.wl.display);
        int ret = poll(fds, sizeof(fds) / sizeof(fds[0]), timeout_ms);

        if (ret == -1) {
            if (errno == EINTR)
                continue;

            LOG_ERRNO("failed to poll file descriptors");
            break;
        }

        if (ret == 0 || (timeout_ms != -1 && !(fds[1].revents & POLLIN))) {
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

        if (fds[3].revents & POLLIN) {
            uint64_t expiration_count;
            ssize_t ret = read(
                term.flash.fd, &expiration_count, sizeof(expiration_count));

            if (ret < 0)
                LOG_ERRNO("failed to read flash timer");
            else
                LOG_DBG("flash timer expired %llu times",
                        (unsigned long long)expiration_count);

            term.flash.active = false;
            term_damage_view(&term);
            if (term.frame_callback == NULL)
                grid_render(&term);
        }

        if (fds[4].revents & POLLIN) {
            uint64_t expiration_count;
            ssize_t ret = read(
                term.blink.fd, &expiration_count, sizeof(expiration_count));

            if (ret < 0)
                LOG_ERRNO("failed to read blink timer");
            else
                LOG_DBG("blink timer expired %llu times",
                        (unsigned long long)expiration_count);

            term.blink.state = term.blink.state == BLINK_ON
                ? BLINK_OFF : BLINK_ON;

            /* Scan all visible cells and mark rows with blinking cells dirty */
            for (int r = 0; r < term.rows; r++) {
                struct row *row = grid_row_in_view(term.grid, r);
                for (int col = 0; col < term.cols; col++) {
                    if (row->cells[col].attrs.blink) {
                        row->dirty = true;
                        break;
                    }
                }
            }

            if (term.frame_callback == NULL)
                grid_render(&term);
        }

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
    if (term.selection.clipboard.data_source != NULL)
        wl_data_source_destroy(term.selection.clipboard.data_source);
    if (term.selection.clipboard.data_offer != NULL)
        wl_data_offer_destroy(term.selection.clipboard.data_offer);
    free(term.selection.clipboard.text);
    if (term.wl.data_device != NULL)
        wl_data_device_destroy(term.wl.data_device);
    if (term.wl.data_device_manager != NULL)
        wl_data_device_manager_destroy(term.wl.data_device_manager);
    if (term.selection.primary.data_source != NULL)
        zwp_primary_selection_source_v1_destroy(term.selection.primary.data_source);
    if (term.selection.primary.data_offer != NULL)
        zwp_primary_selection_offer_v1_destroy(term.selection.primary.data_offer);
    free(term.selection.primary.text);
    if (term.wl.primary_selection_device != NULL)
        zwp_primary_selection_device_v1_destroy(term.wl.primary_selection_device);
    if (term.wl.primary_selection_device_manager != NULL)
        zwp_primary_selection_device_manager_v1_destroy(term.wl.primary_selection_device_manager);
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

    free(term.vt.osc.data);
    for (int row = 0; row < term.normal.num_rows; row++)
        grid_row_free(term.normal.rows[row]);
    free(term.normal.rows);
    for (int row = 0; row < term.alt.num_rows; row++)
        grid_row_free(term.alt.rows[row]);
    free(term.alt.rows);

    free(term.window_title);
    tll_free_and_free(term.window_title_stack, free);

    for (size_t i = 0; i < sizeof(term.fonts) / sizeof(term.fonts[0]); i++) {
        struct font *f = &term.fonts[i];

        if (f->font != NULL)
            cairo_scaled_font_destroy(f->font);

        for (size_t j = 0; j < 256; j++)
            cairo_glyph_free(f->glyph_cache[j].glyphs);
    }

    if (term.flash.fd != -1)
        close(term.flash.fd);
    if (term.blink.fd != -1)
        close(term.blink.fd);

    if (term.ptmx != -1)
        close(term.ptmx);

    thrd_join(keyboard_repeater_id, NULL);
    cnd_destroy(&term.kbd.repeat.cond);
    mtx_destroy(&term.kbd.repeat.mutex);
    close(term.kbd.repeat.pipe_read_fd);
    close(term.kbd.repeat.pipe_write_fd);

    config_free(conf);

    cairo_debug_reset_static_data();
    return ret;
}
