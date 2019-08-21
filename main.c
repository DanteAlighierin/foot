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
#include <sys/sysinfo.h>
#include <sys/prctl.h>

#include <freetype/tttables.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-shell.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <xdg-output-unstable-v1.h>

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
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                int32_t physical_width, int32_t physical_height,
                int32_t subpixel, const char *make, const char *model,
                int32_t transform)
{
    struct monitor *mon = data;
    mon->width_mm = physical_width;
    mon->height_mm = physical_height;
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
            int32_t width, int32_t height, int32_t refresh)
{
}

static void
output_done(void *data, struct wl_output *wl_output)
{
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    struct monitor *mon = data;
    mon->scale = factor;
    render_reload_cursor_theme(mon->term);
    render_resize(mon->term, mon->term->width, mon->term->height);
}

static const struct wl_output_listener output_listener = {
    .geometry = &output_geometry,
    .mode = &output_mode,
    .done = &output_done,
    .scale = &output_scale,
};

static void
xdg_output_handle_logical_position(
    void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y)
{
    struct monitor *mon = data;
    mon->x = x;
    mon->y = y;
}

static void
xdg_output_handle_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
                               int32_t width, int32_t height)
{
    struct monitor *mon = data;
    mon->width_px = width;
    mon->height_px = height;
}

static void
xdg_output_handle_done(void *data, struct zxdg_output_v1 *xdg_output)
{
}

static void
xdg_output_handle_name(void *data, struct zxdg_output_v1 *xdg_output,
                       const char *name)
{
    struct monitor *mon = data;
    mon->name = strdup(name);
}

static void
xdg_output_handle_description(void *data, struct zxdg_output_v1 *xdg_output,
                              const char *description)
{
}

static struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_handle_logical_position,
    .logical_size = xdg_output_handle_logical_size,
    .done = xdg_output_handle_done,
    .name = xdg_output_handle_name,
    .description = xdg_output_handle_description,
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
            term->wl.registry, name, &wl_seat_interface, 5);
        wl_seat_add_listener(term->wl.seat, &seat_listener, term);
        wl_display_roundtrip(term->wl.display);
    }

    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        term->wl.xdg_output_manager = wl_registry_bind(
            term->wl.registry, name, &zxdg_output_manager_v1_interface, 2);
    }

    else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *output = wl_registry_bind(
            term->wl.registry, name, &wl_output_interface, 3);

        tll_push_back(
            term->wl.monitors, ((struct monitor){
                    .term = term, .output = output}));

        struct monitor *mon = &tll_back(term->wl.monitors);
        wl_output_add_listener(output, &output_listener, mon);

        mon->xdg = zxdg_output_manager_v1_get_xdg_output(
            term->wl.xdg_output_manager, mon->output);
        zxdg_output_v1_add_listener(mon->xdg, &xdg_output_listener, mon);
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
surface_enter(void *data, struct wl_surface *wl_surface,
              struct wl_output *wl_output)
{
    struct terminal *term = data;
    tll_foreach(term->wl.monitors, it) {
        if (it->item.output == wl_output) {
            LOG_DBG("mapped on %s", it->item.name);
            tll_push_back(term->wl.on_outputs, &it->item);

            /* Resize, since scale-to-use may have changed */
            render_reload_cursor_theme(term);
            render_resize(term, term->width, term->height);
            return;
        }
    }

    LOG_ERR("mapped on unknown output");
}

static void
surface_leave(void *data, struct wl_surface *wl_surface,
              struct wl_output *wl_output)
{
    struct terminal *term = data;
    tll_foreach(term->wl.on_outputs, it) {
        if (it->item->output != wl_output)
            continue;

        LOG_DBG("unmapped from %s", it->item->name);
        tll_remove(term->wl.on_outputs, it);

        /* Resize, since scale-to-use may have changed */
        render_reload_cursor_theme(term);
        render_resize(term, term->width, term->height);
        return;
    }

    LOG_ERR("unmapped from unknown output");
}

static const struct wl_surface_listener surface_listener = {
    .enter = &surface_enter,
    .leave = &surface_leave,
};

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                       int32_t width, int32_t height, struct wl_array *states)
{
    LOG_DBG("xdg-toplevel: configure: %dx%d", width, height);

    if (width <= 0 || height <= 0)
        return;

    struct terminal *term = data;
    render_resize(term, width, height);
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

static void
print_usage(const char *prog_name)
{
    printf("Usage: %s [OPTION]...\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -f,--font=FONT             font name and style in fontconfig format (monospace)\n"
           "  -t,--term=TERM             value to set the environment variable TERM to (foot)\n"
           "  -v,--version               show the version number and quit\n");
    printf("\n");
}

int
main(int argc, char *const *argv)
{
    int ret = EXIT_FAILURE;

    struct config conf = {NULL};
    if (!config_load(&conf))
        return ret;

    const char *const prog_name = argv[0];

    static const struct option longopts[] =  {
        {"term",    required_argument, 0, 't'},
        {"font",    required_argument, 0, 'f'},
        {"version", no_argument,       0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {NULL,      no_argument,       0,   0},
    };

    while (true) {
        int c = getopt_long(argc, argv, ":t:f:vh", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 't':
            free(conf.term);
            conf.term = strdup(optarg);
            break;

        case 'f':
            tll_free_and_free(conf.fonts, free);
            tll_push_back(conf.fonts, strdup(optarg));
            break;

        case 'v':
            printf("foot version %s\n", FOOT_VERSION);
            return EXIT_SUCCESS;

        case 'h':
            print_usage(prog_name);
            return EXIT_SUCCESS;

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

    struct terminal term = {
        .quit = false,
        .ptmx = posix_openpt(O_RDWR | O_NOCTTY),
        .cursor_keys_mode = CURSOR_KEYS_NORMAL,
        .keypad_keys_mode = KEYPAD_NUMERICAL,
        .auto_margin = true,
        .window_title_stack = tll_init(),
        .flash = {
            .fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK),
        },
        .blink = {
            .fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK),
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
                .fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK),
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
            .alpha = conf.colors.alpha,
        },
        .default_cursor_style = conf.cursor.style,
        .cursor_style = conf.cursor.style,
        .default_cursor_color = {
            .text = conf.cursor.color.text,
            .cursor = conf.cursor.color.cursor,
        },
        .cursor_color = {
            .text = conf.cursor.color.text,
            .cursor = conf.cursor.color.cursor,
        },
        .selection = {
            .start = {-1, -1},
            .end = {-1, -1},
        },
        .normal = {.damage = tll_init(), .scroll_damage = tll_init()},
        .alt = {.damage = tll_init(), .scroll_damage = tll_init()},
        .grid = &term.normal,
        .render = {
            .scrollback_lines = conf.scrollback_lines,
            .workers = {
                .count = conf.render_worker_count,
                .queue = tll_init(),
            },
        },
    };

    LOG_INFO("using %zu rendering threads", term.render.workers.count);

    struct render_worker_context worker_context[term.render.workers.count];

    /* Initialize 'current' colors from the default colors */
    term.colors.fg = term.colors.default_fg;
    term.colors.bg = term.colors.default_bg;
    for (size_t i = 0; i < 8; i++) {
        term.colors.regular[i] = term.colors.default_regular[i];
        term.colors.bright[i] = term.colors.default_bright[i];
    }

    if (term.ptmx == -1) {
        LOG_ERR("failed to open pseudo terminal");
        goto out;
    }

    if (term.flash.fd == -1 || term.blink.fd == -1 || term.kbd.repeat.fd == -1) {
        LOG_ERR("failed to create timers");
        goto out;
    }

    sem_init(&term.render.workers.start, 0, 0);
    sem_init(&term.render.workers.done, 0, 0);
    mtx_init(&term.render.workers.lock, mtx_plain);
    cnd_init(&term.render.workers.cond);

    term.render.workers.threads = calloc(term.render.workers.count, sizeof(term.render.workers.threads[0]));
    for (size_t i = 0; i < term.render.workers.count; i++) {
        worker_context[i].term = &term;
        worker_context[i].my_id = 1 + i;
        thrd_create(&term.render.workers.threads[i], &render_worker_thread, &worker_context[i]);
    }

    font_list_t font_names = tll_init();
    tll_foreach(conf.fonts, it)
        tll_push_back(font_names, it->item);

    if (!font_from_name(font_names, "", &term.fonts[0])) {
        tll_free(font_names);
        goto out;
    }

    font_from_name(font_names, "style=bold", &term.fonts[1]);
    font_from_name(font_names, "style=italic", &term.fonts[2]);
    font_from_name(font_names, "style=bold italic", &term.fonts[3]);

    tll_free(font_names);

    /* Underline position and size */
    for (size_t i = 0; i < sizeof(term.fonts) / sizeof(term.fonts[0]); i++) {
        struct font *f = &term.fonts[i];

        if (f->face == NULL)
            continue;

        FT_Face ft_face = f->face;

        double x_scale = ft_face->size->metrics.x_scale / 65526.;
        double height = ft_face->size->metrics.height / 64;
        double descent = ft_face->size->metrics.descender / 64;

        LOG_DBG("ft: x-scale: %f, height: %f, descent: %f",
                x_scale, height, descent);

        f->underline.position = round(ft_face->underline_position * x_scale / 64.);
        f->underline.thickness = ceil(ft_face->underline_thickness * x_scale / 64.);

        if (f->underline.position == 0.) {
            f->underline.position =  descent / 2.;
            f->underline.thickness =  fabs(round(descent / 5.));
        }

        LOG_DBG("underline: pos=%d, thick=%d",
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

        LOG_DBG("strikeout: pos=%d, thick=%d",
                f->strikeout.position, f->strikeout.thickness);
    }

    {
        FT_Face ft_face = term.fonts[0].face;
        int max_x_advance = ft_face->size->metrics.max_advance / 64;
        int height = ft_face->size->metrics.height / 64;
        int descent = ft_face->size->metrics.descender / 64;
        int ascent = ft_face->size->metrics.ascender / 64;

        term.fextents.height = height * term.fonts[0].pixel_size_fixup;
        term.fextents.descent = -descent * term.fonts[0].pixel_size_fixup;
        term.fextents.ascent = ascent * term.fonts[0].pixel_size_fixup;
        term.fextents.max_x_advance = max_x_advance * term.fonts[0].pixel_size_fixup;

        LOG_DBG("metrics: height: %d, descent: %d, ascent: %d, x-advance: %d",
                height, descent, ascent, max_x_advance);
    }

    term.cell_width = (int)ceil(term.fextents.max_x_advance);
    term.cell_height = (int)ceil(term.fextents.height);
    LOG_DBG("cell width=%d, height=%d", term.cell_width, term.cell_height);

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

    tll_foreach(term.wl.monitors, it) {
        LOG_INFO("%s: %dx%d+%dx%d (scale=%d)",
                 it->item.name, it->item.width_px, it->item.height_px,
                 it->item.x, it->item.y, it->item.scale);
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

    /* Note: theme is (re)loaded on scale and output changes */
    LOG_INFO("cursor theme: %s, size: %u", cursor_theme, cursor_size);
    term.wl.pointer.size = cursor_size;
    term.wl.pointer.theme_name = cursor_theme != NULL ? strdup(cursor_theme) : NULL;

    term.wl.pointer.surface = wl_compositor_create_surface(term.wl.compositor);
    if (term.wl.pointer.surface == NULL) {
        LOG_ERR("failed to create cursor surface");
        goto out;
    }

    term.wl.surface = wl_compositor_create_surface(term.wl.compositor);
    if (term.wl.surface == NULL) {
        LOG_ERR("failed to create wayland surface");
        goto out;
    }

    wl_surface_add_listener(term.wl.surface, &surface_listener, &term);

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


    {
        int fd = wl_display_get_fd(term.wl.display);
        int fd_flags = fcntl(fd, F_GETFL);
        if (fd_flags == -1) {
            LOG_ERRNO("failed to set non blocking mode on Wayland display connection");
            goto out;
        }
        if (fcntl(fd, F_SETFL, fd_flags | O_NONBLOCK) == -1) {
            LOG_ERRNO("failed to set non blocking mode on Wayland display connection");
            goto out;
        }
    }


    int timeout_ms = -1;
    while (true) {
        struct pollfd fds[] = {
            {.fd = wl_display_get_fd(term.wl.display), .events = POLLIN},
            {.fd = term.ptmx,                     .events = POLLIN},
            {.fd = term.kbd.repeat.fd,            .events = POLLIN},
            {.fd = term.flash.fd,                 .events = POLLIN},
            {.fd = term.blink.fd,                 .events = POLLIN},
        };

        wl_display_flush(term.wl.display);
        int pret = poll(fds, sizeof(fds) / sizeof(fds[0]), timeout_ms);

        if (pret == -1) {
            if (errno == EINTR)
                continue;

            LOG_ERRNO("failed to poll file descriptors");
            break;
        }

        if (pret == 0 || (timeout_ms != -1 && !(fds[1].revents & POLLIN))) {
            /* Delayed rendering */
            render_refresh(&term);
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
            uint8_t data[24 * 1024];
            ssize_t count = read(term.ptmx, data, sizeof(data));
            if (count < 0 && errno != EAGAIN) {
                LOG_ERRNO("failed to read from pseudo terminal");
                break;
            }

            if (count > 0) {
                vt_from_slave(&term, data, count);

                /*
                 * We likely need to re-render. But, we don't want to
                 * do it immediately. Often, a single client operation
                 * is done through multiple writes. Many times, we're
                 * so fast that we render mid-operation frames.
                 *
                 * For example, we might end up rendering a frame
                 * where the client just erased a line, while in the
                 * next frame, the client wrote to the same line. This
                 * causes screen "flashes".
                 *
                 * Mitigate by always incuring a small delay before
                 * rendering the next frame. This gives the client
                 * some time to finish the operation (and thus gives
                 * us time to receive the last writes before doing any
                 * actual rendering).
                 *
                 * Note that when the client is producing data at a
                 * very high pace, we're rate limited by the wayland
                 * compositor anyway. The delay we introduce here only
                 * has any effect when the renderer is idle.
                 *
                 * TODO: this adds input latency. Can we somehow hint
                 * ourselves we just received keyboard input, and in
                 * this case *not* delay rendering?
                 */
                timeout_ms = 1;
            }
        }

        if (fds[1].revents & POLLHUP) {
            ret = EXIT_SUCCESS;
            break;
        }

        if (fds[2].revents & POLLIN) {
            uint64_t expiration_count;
            ssize_t ret = read(
                term.kbd.repeat.fd, &expiration_count, sizeof(expiration_count));

            if (ret < 0 && errno != EAGAIN)
                LOG_ERRNO("failed to read repeat key from repeat timer fd");
            else if (ret > 0) {
                term.kbd.repeat.dont_re_repeat = true;
                for (size_t i = 0; i < expiration_count; i++)
                    input_repeat(&term, term.kbd.repeat.key);
                term.kbd.repeat.dont_re_repeat = false;
            }
        }

        if (fds[3].revents & POLLIN) {
            uint64_t expiration_count;
            ssize_t ret = read(
                term.flash.fd, &expiration_count, sizeof(expiration_count));

            if (ret < 0 && errno != EAGAIN)
                LOG_ERRNO("failed to read flash timer");
            else if (ret > 0) {
                LOG_DBG("flash timer expired %llu times",
                        (unsigned long long)expiration_count);

                term.flash.active = false;
                term_damage_view(&term);
                render_refresh(&term);
            }
        }

        if (fds[4].revents & POLLIN) {
            uint64_t expiration_count;
            ssize_t ret = read(
                term.blink.fd, &expiration_count, sizeof(expiration_count));

            if (ret < 0 && errno != EAGAIN)
                LOG_ERRNO("failed to read blink timer");
            else if (ret > 0) {
                LOG_DBG("blink timer expired %llu times",
                        (unsigned long long)expiration_count);

                term.blink.state = term.blink.state == BLINK_ON
                    ? BLINK_OFF : BLINK_ON;

                /* Scan all visible cells and mark rows with blinking cells dirty */
                for (int r = 0; r < term.rows; r++) {
                    struct row *row = grid_row_in_view(term.grid, r);
                    for (int col = 0; col < term.cols; col++) {
                        struct cell *cell = &row->cells[col];

                        if (cell->attrs.blink) {
                            cell->attrs.clean = 0;
                            row->dirty = true;
                        }
                    }
                }

                render_refresh(&term);
            }
        }

    }

out:
    mtx_lock(&term.render.workers.lock);
    assert(tll_length(term.render.workers.queue) == 0);
    for (size_t i = 0; i < term.render.workers.count; i++) {
        sem_post(&term.render.workers.start);
        tll_push_back(term.render.workers.queue, -2);
    }
    cnd_broadcast(&term.render.workers.cond);
    mtx_unlock(&term.render.workers.lock);

    shm_fini();

    tll_free(term.wl.on_outputs);
    tll_foreach(term.wl.monitors, it) {
        free(it->item.name);
        if (it->item.xdg != NULL)
            zxdg_output_v1_destroy(it->item.xdg);
        if (it->item.output != NULL)
            wl_output_destroy(it->item.output);
        tll_remove(term.wl.monitors, it);
    }

    if (term.wl.xdg_output_manager != NULL)
        zxdg_output_manager_v1_destroy(term.wl.xdg_output_manager);

    if (term.render.frame_callback != NULL)
        wl_callback_destroy(term.render.frame_callback);
    if (term.wl.xdg_toplevel != NULL)
        xdg_toplevel_destroy(term.wl.xdg_toplevel);
    if (term.wl.xdg_surface != NULL)
        xdg_surface_destroy(term.wl.xdg_surface);
    free(term.wl.pointer.theme_name);
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

    for (size_t i = 0; i < sizeof(term.fonts) / sizeof(term.fonts[0]); i++)
        font_destroy(&term.fonts[i]);

    if (term.flash.fd != -1)
        close(term.flash.fd);
    if (term.blink.fd != -1)
        close(term.blink.fd);
    if (term.kbd.repeat.fd != -1)
        close(term.kbd.repeat.fd);

    if (term.ptmx != -1)
        close(term.ptmx);

    for (size_t i = 0; i < term.render.workers.count; i++)
        thrd_join(term.render.workers.threads[i], NULL);
    free(term.render.workers.threads);
    cnd_destroy(&term.render.workers.cond);
    mtx_destroy(&term.render.workers.lock);
    sem_destroy(&term.render.workers.start);
    sem_destroy(&term.render.workers.done);
    assert(tll_length(term.render.workers.queue) == 0);
    tll_free(term.render.workers.queue);

    config_free(conf);
    return ret;

}
