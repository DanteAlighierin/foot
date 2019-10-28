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
#include <sys/wait.h>
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
#include "render.h"
#include "shm.h"
#include "slave.h"
#include "terminal.h"
#include "tokenize.h"
#include "version.h"
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


    struct fdm *fdm = NULL;
    struct wayland *wayl = NULL;
    struct terminal *term = NULL;

    if ((fdm = fdm_init()) == NULL)
        goto out;

    if ((wayl = wayl_init(fdm)) == NULL)
        goto out;

    if ((term = term_init(&conf, fdm, wayl, argc, argv)) == NULL)
        goto out;

    fdm_add(fdm, term->ptmx, EPOLLIN, &fdm_ptmx, term);
    fdm_add(fdm, term->flash.fd, EPOLLIN, &fdm_flash, term);
    fdm_add(fdm, term->blink.fd, EPOLLIN, &fdm_blink, term);
    fdm_add(fdm, term->delayed_render_timer.lower_fd, EPOLLIN, &fdm_delayed_render, term);
    fdm_add(fdm, term->delayed_render_timer.upper_fd, EPOLLIN, &fdm_delayed_render, term);

    while (true) {
        wl_display_flush(term->wl->display);  /* TODO: figure out how to get rid of this */

        if (!fdm_poll(fdm))
            break;
    }

    if (term->quit)
        ret = EXIT_SUCCESS;

out:
    if (fdm != NULL) {
        fdm_del(fdm, term->ptmx);
        fdm_del(fdm, term->flash.fd);
        fdm_del(fdm, term->blink.fd);
        fdm_del(fdm, term->delayed_render_timer.lower_fd);
        fdm_del(fdm, term->delayed_render_timer.upper_fd);
    }

    shm_fini();

    int child_ret = term_destroy(term);
    wayl_destroy(wayl);
    fdm_destroy(fdm);
    config_free(conf);

    return ret == EXIT_SUCCESS ? child_ret : ret;

}
