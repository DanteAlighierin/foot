#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
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
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/epoll.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-shell.h>

#include <xdg-output-unstable-v1.h>
#include <xdg-decoration-unstable-v1.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 1
#include "log.h"

#include "config.h"
#include "fdm.h"
#include "font.h"
#include "grid.h"
#include "shm.h"
#include "slave.h"
#include "terminal.h"
#include "tokenize.h"
#include "version.h"
#include "render.h"
#include "vt.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

static bool
fdm_ptmx(struct fdm *fdm, int fd, int events, void *data)
{
    struct terminal *term = data;

    if (events & EPOLLHUP) {
        term->quit = true;

        if (!(events & EPOLLIN))
            return false;
    }

    assert(events & EPOLLIN);

    uint8_t buf[24 * 1024];
    ssize_t count = read(term->ptmx, buf, sizeof(buf));

    if (count < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read from pseudo terminal");
        return false;
    }

    vt_from_slave(term, buf, count);

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
     * We incur this delay *every* time we receive
     * input. To ensure we don't delay rendering
     * indefinitely, we start a second timer that is only
     * reset when we render.
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
    if (term->window->frame_callback == NULL) {
        /* First timeout - reset each time we receive input. */
        timerfd_settime(
            term->delayed_render_timer.lower_fd, 0,
            &(struct itimerspec){.it_value = {.tv_nsec = 1000000}},
            NULL);

        /* Second timeout - only reset when we render. Set to one
         * frame (assuming 60Hz) */
        if (!term->delayed_render_timer.is_armed) {
            timerfd_settime(
                term->delayed_render_timer.upper_fd, 0,
                &(struct itimerspec){.it_value = {.tv_nsec = 16666666}},
                NULL);
            term->delayed_render_timer.is_armed = true;
        }
    }

    return !(events & EPOLLHUP);
}

static bool
fdm_flash(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct terminal *term = data;
    uint64_t expiration_count;
    ssize_t ret = read(
        term->flash.fd, &expiration_count, sizeof(expiration_count));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read flash timer");
        return false;
    }

    LOG_DBG("flash timer expired %llu times",
            (unsigned long long)expiration_count);

    term->flash.active = false;
    term_damage_view(term);
    render_refresh(term);
    return true;
}

static bool
fdm_blink(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct terminal *term = data;
    uint64_t expiration_count;
    ssize_t ret = read(
        term->blink.fd, &expiration_count, sizeof(expiration_count));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read blink timer");
        return false;
    }

    LOG_DBG("blink timer expired %llu times",
            (unsigned long long)expiration_count);

    term->blink.state = term->blink.state == BLINK_ON
        ? BLINK_OFF : BLINK_ON;

    /* Scan all visible cells and mark rows with blinking cells dirty */
    for (int r = 0; r < term->rows; r++) {
        struct row *row = grid_row_in_view(term->grid, r);
        for (int col = 0; col < term->cols; col++) {
            struct cell *cell = &row->cells[col];

            if (cell->attrs.blink) {
                cell->attrs.clean = 0;
                row->dirty = true;
            }
        }
    }

    render_refresh(term);
    return true;
}

static bool
fdm_delayed_render(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct terminal *term = data;
    assert(term->delayed_render_timer.is_armed);

    uint64_t unused;
    ssize_t ret1 = 0;
    ssize_t ret2 = 0;

    if (fd == term->delayed_render_timer.lower_fd)
        ret1 = read(term->delayed_render_timer.lower_fd, &unused, sizeof(unused));
    if (fd == term->delayed_render_timer.upper_fd)
        ret2 = read(term->delayed_render_timer.upper_fd, &unused, sizeof(unused));

    if ((ret1 < 0 || ret2 < 0) && errno != EAGAIN)
        LOG_ERRNO("failed to read timeout timer");
    else if (ret1 > 0 || ret2 > 0) {
        render_refresh(term);

        /* Reset timers */
        term->delayed_render_timer.is_armed = false;
        timerfd_settime(term->delayed_render_timer.lower_fd, 0, &(struct itimerspec){.it_value = {0}}, NULL);
        timerfd_settime(term->delayed_render_timer.upper_fd, 0, &(struct itimerspec){.it_value = {0}}, NULL);
    } else
        assert(false);

    return true;
}

static void
print_usage(const char *prog_name)
{
    printf("Usage: %s [OPTION]...\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -f,--font=FONT              comma separated list of fonts in fontconfig format (monospace)\n"
           "  -t,--term=TERM              value to set the environment variable TERM to (foot)\n"
           "  -g,--geometry=WIDTHxHEIGHT  set initial width and height\n"
           "  -v,--version                show the version number and quit\n");
    printf("\n");
}

int
main(int argc, char *const *argv)
{
    int ret = EXIT_FAILURE;

    /* Startup notifications; we don't support it, but must ensure we
     * don't pass this on to programs launched by us */
    unsetenv("DESKTOP_STARTUP_ID");

    struct config conf = {NULL};
    if (!config_load(&conf))
        return ret;

    const char *const prog_name = argv[0];

    static const struct option longopts[] =  {
        {"term",     required_argument, 0, 't'},
        {"font",     required_argument, 0, 'f'},
        {"geometry", required_argument, 0, 'g'},
        {"version",  no_argument,       0, 'v'},
        {"help",     no_argument,       0, 'h'},
        {NULL,       no_argument,       0,   0},
    };

    while (true) {
        int c = getopt_long(argc, argv, ":t:f:g:vh", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 't':
            free(conf.term);
            conf.term = strdup(optarg);
            break;

        case 'f':
            tll_free_and_free(conf.fonts, free);
            for (char *font = strtok(optarg, ","); font != NULL; font = strtok(NULL, ",")) {

                /* Strip leading spaces */
                while (*font != '\0' && isspace(*font))
                    font++;

                /* Strip trailing spaces */
                char *end = font + strlen(font);
                assert(*end == '\0');
                end--;
                while (end > font && isspace(*end))
                    *(end--) = '\0';

                if (strlen(font) == 0)
                    continue;

                tll_push_back(conf.fonts, strdup(font));
            }
            break;

        case 'g': {
            unsigned width, height;
            if (sscanf(optarg, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
                fprintf(stderr, "error: invalid geometry: %s\n", optarg);
                return EXIT_FAILURE;
            }

            conf.width = width;
            conf.height = height;
            break;
        }

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
        .scale = 1,
        .flash = {
            .fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK),
        },
        .blink = {
            .fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK),
        },
        .vt = {
            .state = 1,  /* STATE_GROUND */
        },
        .colors = {
            .default_fg = conf.colors.fg,
            .default_bg = conf.colors.bg,
            .default_table = {
                conf.colors.regular[0],
                conf.colors.regular[1],
                conf.colors.regular[2],
                conf.colors.regular[3],
                conf.colors.regular[4],
                conf.colors.regular[5],
                conf.colors.regular[6],
                conf.colors.regular[7],

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
        .delayed_render_timer = {
            .is_armed = false,
            .lower_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC),
            .upper_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC),
        },
    };

    LOG_INFO("using %zu rendering threads", term.render.workers.count);

    struct render_worker_context worker_context[term.render.workers.count];

    /* Initialize 'current' colors from the default colors */
    term.colors.fg = term.colors.default_fg;
    term.colors.bg = term.colors.default_bg;

    /* Initialize the 256 gray-scale color cube */
    {
        /* First 16 entries have already been initialized from conf */
        for (size_t r = 0; r < 6; r++) {
            for (size_t g = 0; g < 6; g++) {
                for (size_t b = 0; b < 6; b++) {
                    term.colors.default_table[16 + r * 6 * 6 + g * 6 + b]
                        = r * 51 << 16 | g * 51 << 8 | b * 51;
                }
            }
        }

        for (size_t i = 0; i < 24; i++)
            term.colors.default_table[232 + i] = i * 11 << 16 | i * 11 << 8 | i * 11;

        memcpy(term.colors.table, term.colors.default_table, sizeof(term.colors.table));
    }

    struct fdm *fdm = NULL;
    struct wayland *wayl = NULL;

    if ((fdm = fdm_init()) == NULL)
        goto out;

    if ((wayl = wayl_init(fdm)) == NULL)
        goto out;

    term.wl = wayl;
    wayl->term = &term;

    if (term.ptmx == -1) {
        LOG_ERR("failed to open pseudo terminal");
        goto out;
    }

    if (term.flash.fd == -1 || term.blink.fd == -1) {
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

    if ((term.fonts[0] = font_from_name(font_names, "")) == NULL) {
        tll_free(font_names);
        goto out;
    }

    term.fonts[1] = font_from_name(font_names, "style=bold");
    term.fonts[2] = font_from_name(font_names, "style=italic");
    term.fonts[3] = font_from_name(font_names, "style=bold italic");

    tll_free(font_names);

    {
        FT_Face ft_face = term.fonts[0]->face;
        int max_x_advance = ft_face->size->metrics.max_advance / 64;
        int height = ft_face->size->metrics.height / 64;
        int descent = ft_face->size->metrics.descender / 64;
        int ascent = ft_face->size->metrics.ascender / 64;

        term.fextents.height = height * term.fonts[0]->pixel_size_fixup;
        term.fextents.descent = -descent * term.fonts[0]->pixel_size_fixup;
        term.fextents.ascent = ascent * term.fonts[0]->pixel_size_fixup;
        term.fextents.max_x_advance = max_x_advance * term.fonts[0]->pixel_size_fixup;

        LOG_DBG("metrics: height: %d, descent: %d, ascent: %d, x-advance: %d",
                height, descent, ascent, max_x_advance);
    }

    term.cell_width = (int)ceil(term.fextents.max_x_advance);
    term.cell_height = (int)ceil(term.fextents.height);
    LOG_INFO("cell width=%d, height=%d", term.cell_width, term.cell_height);

    /* Main window */
    term.window = wayl_win_init(wayl);
    if (term.window == NULL)
        goto out;

    term_set_window_title(&term, "foot");

    if (conf.width == -1) {
        assert(conf.height == -1);
        conf.width = 80 * term.cell_width;
        conf.height = 24 * term.cell_height;
    }

    conf.width = max(conf.width, term.cell_width);
    conf.height = max(conf.height, term.cell_height);
    render_resize(&term, conf.width, conf.height);

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

    fdm_add(fdm, term.ptmx, EPOLLIN, &fdm_ptmx, &term);
    fdm_add(fdm, term.flash.fd, EPOLLIN, &fdm_flash, &term);
    fdm_add(fdm, term.blink.fd, EPOLLIN, &fdm_blink, &term);
    fdm_add(fdm, term.delayed_render_timer.lower_fd, EPOLLIN, &fdm_delayed_render, &term);
    fdm_add(fdm, term.delayed_render_timer.upper_fd, EPOLLIN, &fdm_delayed_render, &term);

    while (true) {
        wl_display_flush(term.wl->display);  /* TODO: figure out how to get rid of this */

        if (!fdm_poll(fdm))
            break;
    }

    if (term.quit)
        ret = EXIT_SUCCESS;

out:
    if (fdm != NULL) {
        fdm_del(fdm, term.ptmx);
        fdm_del(fdm, term.flash.fd);
        fdm_del(fdm, term.blink.fd);
        fdm_del(fdm, term.delayed_render_timer.lower_fd);
        fdm_del(fdm, term.delayed_render_timer.upper_fd);
    }

    if (term.delayed_render_timer.lower_fd != -1)
        close(term.delayed_render_timer.lower_fd);
    if (term.delayed_render_timer.upper_fd != -1)
        close(term.delayed_render_timer.upper_fd);

    mtx_lock(&term.render.workers.lock);
    assert(tll_length(term.render.workers.queue) == 0);
    for (size_t i = 0; i < term.render.workers.count; i++) {
        sem_post(&term.render.workers.start);
        tll_push_back(term.render.workers.queue, -2);
    }
    cnd_broadcast(&term.render.workers.cond);
    mtx_unlock(&term.render.workers.lock);

    shm_fini();

    wayl_win_destroy(term.window);
    wayl_destroy(wayl);

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
        font_destroy(term.fonts[i]);

    free(term.search.buf);

    if (term.flash.fd != -1)
        close(term.flash.fd);
    if (term.blink.fd != -1)
        close(term.blink.fd);

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

    if (term.slave > 0) {
        /* Note: we've closed ptmx, so the slave *should* exit... */
        int status;
        waitpid(term.slave, &status, 0);

        int child_ret = EXIT_FAILURE;
        if (WIFEXITED(status)) {
            child_ret = WEXITSTATUS(status);
            LOG_DBG("slave exited with code %d", child_ret);
        } else if (WIFSIGNALED(status)) {
            child_ret = WTERMSIG(status);
            LOG_WARN("slave exited with signal %d", child_ret);
        } else {
            LOG_WARN("slave exited for unknown reason (status = 0x%08x)", status);
        }

        if (ret == EXIT_SUCCESS)
            ret = child_ret;
    }

    fdm_destroy(fdm);

    config_free(conf);
    return ret;

}
