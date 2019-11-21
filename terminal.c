#include "terminal.h"

#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>

#define LOG_MODULE "terminal"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "async.h"
#include "grid.h"
#include "render.h"
#include "vt.h"
#include "selection.h"
#include "config.h"
#include "slave.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

bool
term_to_slave(struct terminal *term, const void *_data, size_t len)
{
    if (tll_length(term->ptmx_buffer) > 0) {
        /* With a non-empty queue, EPOLLOUT has already been enabled */
        goto enqueue_data;
    }

    /*
     * Try a synchronous write first. If we fail to write everything,
     * switch to asynchronous.
     */

    switch (async_write(term->ptmx, _data, len, &(size_t){0})) {
    case ASYNC_WRITE_REMAIN:
        /* Switch to asynchronous mode; let FDM write the remaining data */
        if (!fdm_event_add(term->fdm, term->ptmx, EPOLLOUT))
            return false;
        goto enqueue_data;

    case ASYNC_WRITE_DONE:
        return true;

    case ASYNC_WRITE_ERR:
        LOG_ERRNO("failed to synchronously write %zu bytes to slave", len);
        return false;
    }

    /* Shouldn't get here */
    assert(false);
    return false;

enqueue_data:
    /*
     * We're in asynchronous mode - push data to queue and let the FDM
     * handler take care of it
     */
    {
        void *copy = malloc(len);
        memcpy(copy, _data, len);

        struct ptmx_buffer queued = {
            .data = copy,
            .len = len,
            .idx = 0,
        };
        tll_push_back(term->ptmx_buffer, queued);
    }
    return true;
}

static bool
fdm_ptmx_out(struct fdm *fdm, int fd, int events, void *data)
{
    struct terminal *term = data;

    /* If there is no queued data, then we shouldn't be in asynchronous mode */
    assert(tll_length(term->ptmx_buffer) > 0);

    /* Don't use pop() since we may not be able to write the entire buffer */
    tll_foreach(term->ptmx_buffer, it) {
        switch (async_write(term->ptmx, it->item.data, it->item.len, &it->item.idx)) {
        case ASYNC_WRITE_DONE:
            free(it->item.data);
            tll_remove(term->ptmx_buffer, it);
            break;

        case ASYNC_WRITE_REMAIN:
            /* to_slave() updated it->item.idx */
            return true;

        case ASYNC_WRITE_ERR:
            LOG_ERRNO("failed to asynchronously write %zu bytes to slave",
                      it->item.len - it->item.idx);
            return false;
        }
    }

    /* No more queued data, switch back to synchronous mode */
    fdm_event_del(term->fdm, term->ptmx, EPOLLOUT);
    return true;
}

static bool
fdm_ptmx(struct fdm *fdm, int fd, int events, void *data)
{
    struct terminal *term = data;

    bool pollin = events & EPOLLIN;
    bool pollout = events & EPOLLOUT;
    bool hup = events & EPOLLHUP;

    if (hup) {
        /* TODO: should we *not* ignore pollin? */
        return term_shutdown(term);
    }

    if (pollout) {
        if (!fdm_ptmx_out(fdm, fd, events, data))
            return false;
    }

    if (!pollin)
        return true;

    uint8_t buf[24 * 1024];
    ssize_t count = read(term->ptmx, buf, sizeof(buf));

    if (count < 0) {
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

    if (events & EPOLLHUP)
        return term_shutdown(term);

    return true;
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

    /* Invert blink state */
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
    if (!term->delayed_render_timer.is_armed)
        return true;

    uint64_t unused;
    ssize_t ret1 = 0;
    ssize_t ret2 = 0;

    if (fd == term->delayed_render_timer.lower_fd)
        ret1 = read(term->delayed_render_timer.lower_fd, &unused, sizeof(unused));
    if (fd == term->delayed_render_timer.upper_fd)
        ret2 = read(term->delayed_render_timer.upper_fd, &unused, sizeof(unused));

    if ((ret1 < 0 || ret2 < 0)) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read timeout timer");
        return false;
    }

    render_refresh(term);

    /* Reset timers */
    struct itimerspec reset = {{0}};
    timerfd_settime(term->delayed_render_timer.lower_fd, 0, &reset, NULL);
    timerfd_settime(term->delayed_render_timer.upper_fd, 0, &reset, NULL);
    term->delayed_render_timer.is_armed = false;

    return true;
}

static void
initialize_color_cube(struct terminal *term)
{
    /* First 16 entries have already been initialized from conf */
    for (size_t r = 0; r < 6; r++) {
        for (size_t g = 0; g < 6; g++) {
            for (size_t b = 0; b < 6; b++) {
                term->colors.default_table[16 + r * 6 * 6 + g * 6 + b]
                    = r * 51 << 16 | g * 51 << 8 | b * 51;
            }
        }
    }

    for (size_t i = 0; i < 24; i++)
        term->colors.default_table[232 + i] = i * 11 << 16 | i * 11 << 8 | i * 11;

    memcpy(term->colors.table, term->colors.default_table, sizeof(term->colors.table));
}

static bool
initialize_render_workers(struct terminal *term)
{
    LOG_INFO("using %zu rendering threads", term->render.workers.count);

    sem_init(&term->render.workers.start, 0, 0);
    sem_init(&term->render.workers.done, 0, 0);
    mtx_init(&term->render.workers.lock, mtx_plain);
    cnd_init(&term->render.workers.cond);

    term->render.workers.threads = calloc(
        term->render.workers.count, sizeof(term->render.workers.threads[0]));

    for (size_t i = 0; i < term->render.workers.count; i++) {
        struct render_worker_context *ctx = malloc(sizeof(*ctx));
        *ctx = (struct render_worker_context) {
            .term = term,
            .my_id = 1 + i,
        };

        int ret = thrd_create(
            &term->render.workers.threads[i], &render_worker_thread, ctx);

        if (ret != 0) {
            LOG_ERRNO_P("failed to create render worker thread", ret);
            term->render.workers.threads[i] = 0;
            return false;
        }
    }

    return true;
}

static bool
initialize_fonts(struct terminal *term, const struct config *conf)
{
    font_list_t font_names = tll_init();
    tll_foreach(conf->fonts, it)
        tll_push_back(font_names, it->item);

    if ((term->fonts[0] = font_from_name(font_names, "")) == NULL ||
        (term->fonts[1] = font_from_name(font_names, "weight=bold")) == NULL ||
        (term->fonts[2] = font_from_name(font_names, "slant=italic")) == NULL ||
        (term->fonts[3] = font_from_name(font_names, "weight=bold:slant=italic")) == NULL)
    {
        tll_free(font_names);
        return false;
    }

    tll_free(font_names);

    FT_Face ft_face = term->fonts[0]->face;
    int max_x_advance = ft_face->size->metrics.max_advance / 64;
    int height = ft_face->size->metrics.height / 64;
    int descent = ft_face->size->metrics.descender / 64;
    int ascent = ft_face->size->metrics.ascender / 64;

    term->fextents.height = height * term->fonts[0]->pixel_size_fixup;
    term->fextents.descent = -descent * term->fonts[0]->pixel_size_fixup;
    term->fextents.ascent = ascent * term->fonts[0]->pixel_size_fixup;
    term->fextents.max_x_advance = max_x_advance * term->fonts[0]->pixel_size_fixup;

    LOG_DBG("metrics: height: %d, descent: %d, ascent: %d, x-advance: %d",
            height, descent, ascent, max_x_advance);

    return true;
}

struct terminal *
term_init(const struct config *conf, struct fdm *fdm, struct wayland *wayl,
          const char *term_env, int argc, char *const *argv,
          void (*shutdown_cb)(void *data, int exit_code), void *shutdown_data)
{
    int ptmx = -1;
    int flash_fd = -1;
    int blink_fd = -1;
    int delay_lower_fd = -1;
    int delay_upper_fd = -1;

    struct terminal *term = malloc(sizeof(*term));

    if ((ptmx = posix_openpt(O_RDWR | O_NOCTTY)) == -1) {
        LOG_ERRNO("failed to open PTY");
        goto close_fds;
    }
    if ((flash_fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK)) == -1) {
        LOG_ERRNO("failed to create flash timer FD");
        goto close_fds;
    }
    if ((blink_fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK)) == -1) {
        LOG_ERRNO("failed to create blink timer FD");
        goto close_fds;
    }
    if ((delay_lower_fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK)) == -1 ||
        (delay_upper_fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK)) == -1)
    {
        LOG_ERRNO("failed to create delayed rendering timer FDs");
        goto close_fds;
    }

    int ptmx_flags;
    if ((ptmx_flags = fcntl(ptmx, F_GETFL)) < 0 ||
        fcntl(ptmx, F_SETFL, ptmx_flags | O_NONBLOCK) < 0)
    {
        LOG_ERRNO("failed to configure ptmx as non-blocking");
        goto err;
    }

    if (!fdm_add(fdm, ptmx, EPOLLIN, &fdm_ptmx, term) ||
        !fdm_add(fdm, flash_fd, EPOLLIN, &fdm_flash, term) ||
        !fdm_add(fdm, blink_fd, EPOLLIN, &fdm_blink, term) ||
        !fdm_add(fdm, delay_lower_fd, EPOLLIN, &fdm_delayed_render, term) ||
        !fdm_add(fdm, delay_upper_fd, EPOLLIN, &fdm_delayed_render, term))
    {
        goto err;
    }

    /* Initialize configure-based terminal attributes */
    *term = (struct terminal) {
        .fdm = fdm,
        .quit = false,
        .ptmx = ptmx,
        .ptmx_buffer = tll_init(),
        .cursor_keys_mode = CURSOR_KEYS_NORMAL,
        .keypad_keys_mode = KEYPAD_NUMERICAL,
        .auto_margin = true,
        .window_title_stack = tll_init(),
        .scale = 1,
        .flash = {.fd = flash_fd},
        .blink = {.fd = blink_fd},
        .vt = {
            .state = 1,  /* STATE_GROUND */
        },
        .colors = {
            .fg = conf->colors.fg,
            .bg = conf->colors.bg,
            .default_fg = conf->colors.fg,
            .default_bg = conf->colors.bg,
            .default_table = {
                conf->colors.regular[0],
                conf->colors.regular[1],
                conf->colors.regular[2],
                conf->colors.regular[3],
                conf->colors.regular[4],
                conf->colors.regular[5],
                conf->colors.regular[6],
                conf->colors.regular[7],

                conf->colors.bright[0],
                conf->colors.bright[1],
                conf->colors.bright[2],
                conf->colors.bright[3],
                conf->colors.bright[4],
                conf->colors.bright[5],
                conf->colors.bright[6],
                conf->colors.bright[7],
            },
            .alpha = conf->colors.alpha,
        },
        .origin = ORIGIN_ABSOLUTE,
        .default_cursor_style = conf->cursor.style,
        .cursor_style = conf->cursor.style,
        .default_cursor_color = {
            .text = conf->cursor.color.text,
            .cursor = conf->cursor.color.cursor,
        },
        .cursor_color = {
            .text = conf->cursor.color.text,
            .cursor = conf->cursor.color.cursor,
        },
        .selection = {
            .start = {-1, -1},
            .end = {-1, -1},
        },
        .normal = {.damage = tll_init(), .scroll_damage = tll_init()},
        .alt = {.damage = tll_init(), .scroll_damage = tll_init()},
        .grid = &term->normal,
        .tab_stops = tll_init(),
        .wl = wayl,
        .render = {
            .scrollback_lines = conf->scrollback_lines,
            .workers = {
                .count = conf->render_worker_count,
                .queue = tll_init(),
            },
        },
        .delayed_render_timer = {
            .is_armed = false,
            .lower_fd = delay_lower_fd,
            .upper_fd = delay_upper_fd,
        },
        .shutdown_cb = shutdown_cb,
        .shutdown_data = shutdown_data,
    };

    initialize_color_cube(term);
    if (!initialize_render_workers(term))
        goto err;
    if (!initialize_fonts(term, conf))
        goto err;

    /* Cell dimensions are based on the font metrics. Obviously */
    term->cell_width = (int)ceil(term->fextents.max_x_advance);
    term->cell_height = (int)ceil(term->fextents.height);
    LOG_INFO("cell width=%d, height=%d", term->cell_width, term->cell_height);

    /* Start the slave/client */
    if ((term->slave = slave_spawn(term->ptmx, argc, argv, term_env, conf->shell)) == -1)
        goto err;

    /* Initiailze the Wayland window backend */
    if ((term->window = wayl_win_init(wayl)) == NULL)
        goto err;

    term_set_window_title(term, "foot");

    /* Try to use user-configured window dimentions */
    unsigned width = conf->width;
    unsigned height = conf->height;

    if (width == -1) {
        /* No user-configuration - use 80x24 cells */
        assert(height == -1);
        width = 80 * term->cell_width;
        height = 24 * term->cell_height;
    }

    /* Don't go below a single cell */
    width = max(width, term->cell_width);
    height = max(height, term->cell_height);
    render_resize(term, width, height);

    tll_push_back(wayl->terms, term);
    return term;

err:
    term_destroy(term);
    return NULL;

close_fds:
    fdm_del(fdm, ptmx);
    fdm_del(fdm, flash_fd);
    fdm_del(fdm, blink_fd);
    fdm_del(fdm, delay_lower_fd);
    fdm_del(fdm, delay_upper_fd);

    free(term);
    return NULL;
}

static bool
fdm_shutdown(struct fdm *fdm, int fd, int events, void *data)
{
    LOG_DBG("FDM shutdown");
    struct terminal *term = data;

    /* Kill the event FD */
    fdm_del(term->fdm, fd);

    wayl_win_destroy(term->window);
    term->window = NULL;

    struct wayland *wayl __attribute__((unused)) = term->wl;

    /*
     * Normally we'd get unmapped when we destroy the Wayland
     * above.
     *
     * However, it appears that under certain conditions, those events
     * are deferred (for example, when a screen locker is active), and
     * thus we can get here without having been unmapped.
     */
    if (wayl->focused == term)
        wayl->focused = NULL;
    if (wayl->moused == term)
        wayl->moused = NULL;

    assert(wayl->focused != term);
    assert(wayl->moused != term);

    void (*cb)(void *, int) = term->shutdown_cb;
    void *cb_data = term->shutdown_data;

    int exit_code = term_destroy(term);
    if (cb != NULL)
        cb(cb_data, exit_code);

    return true;
}

bool
term_shutdown(struct terminal *term)
{
    if (term->is_shutting_down)
        return true;

    term->is_shutting_down = true;

    /*
     * Close FDs then postpone self-destruction to the next poll
     * iteration, by creating an event FD that we trigger immediately.
     */

    fdm_del(term->fdm, term->delayed_render_timer.lower_fd);
    fdm_del(term->fdm, term->delayed_render_timer.upper_fd);
    fdm_del(term->fdm, term->blink.fd);
    fdm_del(term->fdm, term->flash.fd);
    fdm_del(term->fdm, term->ptmx);

    term->delayed_render_timer.lower_fd = -1;
    term->delayed_render_timer.upper_fd = -1;
    term->blink.fd = -1;
    term->flash.fd = -1;
    term->ptmx = -1;

    int event_fd = eventfd(0, EFD_CLOEXEC);
    if (event_fd == -1) {
        LOG_ERRNO("failed to create terminal shutdown event FD");
        return false;
    }

    if (!fdm_add(term->fdm, event_fd, EPOLLIN, &fdm_shutdown, term)) {
        close(event_fd);
        return false;
    }

    if (write(event_fd, &(uint64_t){1}, sizeof(uint64_t)) != sizeof(uint64_t)) {
        LOG_ERRNO("failed to send terminal shutdown event");
        fdm_del(term->fdm, event_fd);
        return false;
    }

    return true;
}

static volatile sig_atomic_t alarm_raised;

static void
sig_alarm(int signo)
{
    LOG_DBG("SIGALRM");
    alarm_raised = 1;
}

int
term_destroy(struct terminal *term)
{
    if (term == NULL)
        return 0;

    tll_foreach(term->wl->terms, it) {
        if (it->item == term) {
            tll_remove(term->wl->terms, it);
            break;
        }
    }

    fdm_del(term->fdm, term->delayed_render_timer.lower_fd);
    fdm_del(term->fdm, term->delayed_render_timer.upper_fd);
    fdm_del(term->fdm, term->blink.fd);
    fdm_del(term->fdm, term->flash.fd);
    fdm_del(term->fdm, term->ptmx);

    if (term->window != NULL)
        wayl_win_destroy(term->window);

    mtx_lock(&term->render.workers.lock);
    assert(tll_length(term->render.workers.queue) == 0);

    /* Count livinig threads - we may get here when only some of the
     * threads have been successfully started */
    size_t worker_count = 0;
    for (size_t i = 0; i < term->render.workers.count; i++, worker_count++) {
        if (term->render.workers.threads[i] == 0)
            break;
    }

    for (size_t i = 0; i < worker_count; i++) {
        sem_post(&term->render.workers.start);
        tll_push_back(term->render.workers.queue, -2);
    }
    cnd_broadcast(&term->render.workers.cond);
    mtx_unlock(&term->render.workers.lock);

    free(term->vt.osc.data);
    for (int row = 0; row < term->normal.num_rows; row++)
        grid_row_free(term->normal.rows[row]);
    free(term->normal.rows);
    for (int row = 0; row < term->alt.num_rows; row++)
        grid_row_free(term->alt.rows[row]);
    free(term->alt.rows);

    free(term->window_title);
    tll_free_and_free(term->window_title_stack, free);

    for (size_t i = 0; i < sizeof(term->fonts) / sizeof(term->fonts[0]); i++)
        font_destroy(term->fonts[i]);

    free(term->search.buf);

    for (size_t i = 0; i < term->render.workers.count; i++) {
        if (term->render.workers.threads[i] != 0)
            thrd_join(term->render.workers.threads[i], NULL);
    }
    free(term->render.workers.threads);
    cnd_destroy(&term->render.workers.cond);
    mtx_destroy(&term->render.workers.lock);
    sem_destroy(&term->render.workers.start);
    sem_destroy(&term->render.workers.done);
    assert(tll_length(term->render.workers.queue) == 0);
    tll_free(term->render.workers.queue);

    tll_foreach(term->ptmx_buffer, it)
        free(it->item.data);
    tll_free(term->ptmx_buffer);
    tll_free(term->tab_stops);

    int ret = EXIT_SUCCESS;

    if (term->slave > 0) {
        LOG_DBG("waiting for slave (PID=%u) to die", term->slave);

        /*
         * Note: we've closed ptmx, so the slave *should* exit...
         *
         * But, since it is possible to write clients that ignore
         * this, we need to handle it in *some* way.
         *
         * So, what we do is register a SIGALRM handler, and configure
         * a 2 second alarm. If the slave hasn't died after this time,
         * we send it a SIGTERM, then wait another 2 seconds (using
         * the same alarm mechanism). If it still hasn't died, we send
         * it a SIGKILL.
         *
         * Note that this solution is *not* asynchronous, and any
         * other events etc will be ignored during this time. This of
         * course only applies to a 'foot --server' instance, where
         * there might be other terminals running.
         */
        sigaction(SIGALRM, &(const struct sigaction){.sa_handler = &sig_alarm}, NULL);
        alarm(2);

        int status;
        int kill_signal = SIGTERM;

        while (true) {
            int r = waitpid(term->slave, &status, 0);

            if (r == term->slave)
                break;

            if (r == -1) {
                assert(errno == EINTR);

                if (alarm_raised) {
                    LOG_DBG("slave hasn't died yet, sending: %s (%d)",
                            kill_signal == SIGTERM ? "SIGTERM" : "SIGKILL",
                            kill_signal);

                    kill(term->slave, kill_signal);

                    alarm_raised = 0;
                    if (kill_signal != SIGKILL)
                        alarm(2);

                    kill_signal = SIGKILL;

                }
            }
        }

        /* Cancel alarm */
        alarm(0);
        sigaction(SIGALRM, &(const struct sigaction){.sa_handler = SIG_DFL}, NULL);

        ret = EXIT_FAILURE;
        if (WIFEXITED(status)) {
            ret = WEXITSTATUS(status);
            LOG_DBG("slave exited with code %d", ret);
        } else if (WIFSIGNALED(status)) {
            ret = WTERMSIG(status);
            LOG_WARN("slave exited with signal %d (%s)", ret, strsignal(ret));
        } else {
            LOG_WARN("slave exited for unknown reason (status = 0x%08x)", status);
        }
    }

    free(term);

#if defined(__GLIBC__)
    if (!malloc_trim(0))
        LOG_WARN("failed to trim memory");
#endif

    return ret;
}

void
term_reset(struct terminal *term, bool hard)
{
    term->cursor_keys_mode = CURSOR_KEYS_NORMAL;
    term->keypad_keys_mode = KEYPAD_NUMERICAL;
    term->reverse = false;
    term->hide_cursor = false;
    term->auto_margin = true;
    term->insert_mode = false;
    term->bracketed_paste = false;
    term->focus_events = false;
    term->mouse_tracking = MOUSE_NONE;
    term->mouse_reporting = MOUSE_NORMAL;
    term->charsets.selected = 0;
    term->charsets.set[0] = CHARSET_ASCII;
    term->charsets.set[1] = CHARSET_ASCII;
    term->charsets.set[2] = CHARSET_ASCII;
    term->charsets.set[3] = CHARSET_ASCII;
    term->saved_charsets = term->charsets;
    tll_free_and_free(term->window_title_stack, free);
    free(term->window_title);
    term->window_title = strdup("foot");

    term->scroll_region.start = 0;
    term->scroll_region.end = term->rows;

    free(term->vt.osc.data);
    memset(&term->vt, 0, sizeof(term->vt));
    term->vt.state = 1; /* GROUND */

    if (term->grid == &term->alt) {
        term->grid = &term->normal;
        term_restore_cursor(term);
        selection_cancel(term);
    }

    if (!hard)
        return;

    term->flash.active = false;
    term->blink.active = false;
    term->blink.state = BLINK_ON;
    term->colors.fg = term->colors.default_fg;
    term->colors.bg = term->colors.default_bg;
    for (size_t i = 0; i < 256; i++)
        term->colors.table[i] = term->colors.default_table[i];
    term->cursor.lcf = false;
    term->cursor = (struct cursor){.point = {0, 0}};
    term->saved_cursor = (struct cursor){.point = {0, 0}};
    term->alt_saved_cursor = (struct cursor){.point = {0, 0}};
    term->cursor_style = term->default_cursor_style;
    term->cursor_blinking = false;
    term->cursor_color.text = term->default_cursor_color.text;
    term->cursor_color.cursor = term->default_cursor_color.cursor;
    selection_cancel(term);
    term->normal.offset = term->normal.view = 0;
    term->alt.offset = term->alt.view = 0;
    for (size_t i = 0; i < term->rows; i++) {
        memset(term->normal.rows[i]->cells, 0, term->cols * sizeof(struct cell));
        memset(term->alt.rows[i]->cells, 0, term->cols * sizeof(struct cell));
    }
    for (size_t i = term->rows; i < term->normal.num_rows; i++) {
        grid_row_free(term->normal.rows[i]);
        term->normal.rows[i] = NULL;
    }
    for (size_t i = term->rows; i < term->alt.num_rows; i++) {
        grid_row_free(term->alt.rows[i]);
        term->alt.rows[i] = NULL;
    }
    term->normal.cur_row = term->normal.rows[0];
    term->alt.cur_row = term->alt.rows[0];
    tll_free(term->normal.damage);
    tll_free(term->normal.scroll_damage);
    tll_free(term->alt.damage);
    tll_free(term->alt.scroll_damage);
    term->render.last_cursor.cell = NULL;
    term->render.was_flashing = false;
    term_damage_all(term);
}

void
term_damage_rows(struct terminal *term, int start, int end)
{
    assert(start <= end);
    for (int r = start; r <= end; r++) {
        struct row *row = grid_row(term->grid, r);
        row->dirty = true;
        for (int c = 0; c < term->grid->num_cols; c++)
            row->cells[c].attrs.clean = 0;
    }
}

void
term_damage_rows_in_view(struct terminal *term, int start, int end)
{
    assert(start <= end);
    for (int r = start; r <= end; r++) {
        struct row *row = grid_row_in_view(term->grid, r);
        row->dirty = true;
        for (int c = 0; c < term->grid->num_cols; c++)
            row->cells[c].attrs.clean = 0;
    }
}

void
term_damage_all(struct terminal *term)
{
    term_damage_rows(term, 0, term->rows - 1);
}

void
term_damage_view(struct terminal *term)
{
    term_damage_rows_in_view(term, 0, term->rows - 1);
}

void
term_damage_scroll(struct terminal *term, enum damage_type damage_type,
                   struct scroll_region region, int lines)
{
    if (tll_length(term->grid->scroll_damage) > 0) {
        struct damage *dmg = &tll_back(term->grid->scroll_damage);

        if (dmg->type == damage_type &&
            dmg->scroll.region.start == region.start &&
            dmg->scroll.region.end == region.end)
        {
            dmg->scroll.lines += lines;
            return;
        }
    }
    struct damage dmg = {
        .type = damage_type,
        .scroll = {.region = region, .lines = lines},
    };
    tll_push_back(term->grid->scroll_damage, dmg);
}

static inline void
erase_cell_range(struct terminal *term, struct row *row, int start, int end)
{
    assert(start < term->cols);
    assert(end < term->cols);

    if (unlikely(term->vt.attrs.have_bg)) {
        for (int col = start; col <= end; col++) {
            struct cell *c = &row->cells[col];
            c->wc = 0;
            c->attrs = (struct attributes){.have_bg = 1, .bg = term->vt.attrs.bg};
        }
    } else
        memset(&row->cells[start], 0, (end - start + 1) * sizeof(row->cells[0]));

    row->dirty = true;
}

static inline void
erase_line(struct terminal *term, struct row *row)
{
    erase_cell_range(term, row, 0, term->cols - 1);
}

void
term_erase(struct terminal *term, const struct coord *start, const struct coord *end)
{
    assert(start->row <= end->row);
    assert(start->col <= end->col || start->row < end->row);

    if (start->row == end->row) {
        struct row *row = grid_row(term->grid, start->row);
        erase_cell_range(term, row, start->col, end->col);
        return;
    }

    assert(end->row > start->row);

    erase_cell_range(
        term, grid_row(term->grid, start->row), start->col, term->cols - 1);

    for (int r = start->row + 1; r < end->row; r++)
        erase_line(term, grid_row(term->grid, r));

    erase_cell_range(term, grid_row(term->grid, end->row), 0, end->col);
}

int
term_row_rel_to_abs(const struct terminal *term, int row)
{
    switch (term->origin) {
    case ORIGIN_ABSOLUTE:
        return min(row, term->rows - 1);

    case ORIGIN_RELATIVE:
        return min(row + term->scroll_region.start, term->scroll_region.end - 1);
    }

    assert(false);
    return -1;
}

void
term_cursor_to(struct terminal *term, int row, int col)
{
    assert(row < term->rows);
    assert(col < term->cols);

    term->cursor.lcf = false;

    term->cursor.point.col = col;
    term->cursor.point.row = row;

    term->grid->cur_row = grid_row(term->grid, row);
}

void
term_cursor_home(struct terminal *term)
{
    term_cursor_to(term, term_row_rel_to_abs(term, 0), 0);
}

void
term_cursor_left(struct terminal *term, int count)
{
    int move_amount = min(term->cursor.point.col, count);
    term->cursor.point.col -= move_amount;
    assert(term->cursor.point.col >= 0);
    term->cursor.lcf = false;
}

void
term_cursor_right(struct terminal *term, int count)
{
    int move_amount = min(term->cols - term->cursor.point.col - 1, count);
    term->cursor.point.col += move_amount;
    assert(term->cursor.point.col < term->cols);
    term->cursor.lcf = false;
}

void
term_cursor_up(struct terminal *term, int count)
{
    int top = term->origin == ORIGIN_ABSOLUTE ? 0 : term->scroll_region.start;
    assert(term->cursor.point.row >= top);

    int move_amount = min(term->cursor.point.row - top, count);
    term_cursor_to(term, term->cursor.point.row - move_amount, term->cursor.point.col);
}

void
term_cursor_down(struct terminal *term, int count)
{
    int bottom = term->origin == ORIGIN_ABSOLUTE ? term->rows : term->scroll_region.end;
    assert(bottom >= term->cursor.point.row);

    int move_amount = min(bottom - term->cursor.point.row - 1, count);
    term_cursor_to(term, term->cursor.point.row + move_amount, term->cursor.point.col);
}

void
term_scroll_partial(struct terminal *term, struct scroll_region region, int rows)
{
    LOG_DBG("scroll: rows=%d, region.start=%d, region.end=%d",
            rows, region.start, region.end);

#if 0
    if (rows > region.end - region.start) {
        /* For now, clamp */
        rows = region.end - region.start;
    }
#endif

    bool view_follows = term->grid->view == term->grid->offset;
    term->grid->offset += rows;
    term->grid->offset &= term->grid->num_rows - 1;

    if (view_follows)
        term->grid->view = term->grid->offset;

    /* Top non-scrolling region. */
    for (int i = region.start - 1; i >= 0; i--)
        grid_swap_row(term->grid, i - rows, i, false);

    /* Bottom non-scrolling region */
    for (int i = term->rows - 1; i >= region.end; i--)
        grid_swap_row(term->grid, i - rows, i, false);

    /* Erase scrolled in lines */
    for (int r = max(region.end - rows, region.start); r < region.end; r++) {
        erase_line(term, grid_row_and_alloc(term->grid, r));
        if (selection_on_row_in_view(term, r))
            selection_cancel(term);
    }

    term_damage_scroll(term, DAMAGE_SCROLL, region, rows);
    term->grid->cur_row = grid_row(term->grid, term->cursor.point.row);
}

void
term_scroll(struct terminal *term, int rows)
{
    term_scroll_partial(term, term->scroll_region, rows);
}

void
term_scroll_reverse_partial(struct terminal *term,
                            struct scroll_region region, int rows)
{
    LOG_DBG("scroll reverse: rows=%d, region.start=%d, region.end=%d",
            rows, region.start, region.end);

#if 0
    if (rows > region.end - region.start) {
        /* For now, clamp */
        rows = region.end - region.start;
    }
#endif

    bool view_follows = term->grid->view == term->grid->offset;
    term->grid->offset -= rows;
    while (term->grid->offset < 0)
        term->grid->offset += term->grid->num_rows;
    term->grid->offset &= term->grid->num_rows - 1;

    assert(term->grid->offset >= 0);
    assert(term->grid->offset < term->grid->num_rows);

    if (view_follows)
        term->grid->view = term->grid->offset;

    /* Bottom non-scrolling region */
    for (int i = region.end + rows; i < term->rows + rows; i++)
        grid_swap_row(term->grid, i, i - rows, false);

    /* Top non-scrolling region */
    for (int i = 0 + rows; i < region.start + rows; i++)
        grid_swap_row(term->grid, i, i - rows, false);

    /* Erase scrolled in lines */
    for (int r = region.start; r < min(region.start + rows, region.end); r++) {
        erase_line(term, grid_row_and_alloc(term->grid, r));
        if (selection_on_row_in_view(term, r))
            selection_cancel(term);
    }

    term_damage_scroll(term, DAMAGE_SCROLL_REVERSE, region, rows);
    term->grid->cur_row = grid_row(term->grid, term->cursor.point.row);
}

void
term_scroll_reverse(struct terminal *term, int rows)
{
    term_scroll_reverse_partial(term, term->scroll_region, rows);
}

void
term_linefeed(struct terminal *term)
{
    if (term->cursor.point.row == term->scroll_region.end - 1)
        term_scroll(term, 1);
    else
        term_cursor_down(term, 1);
}

void
term_reverse_index(struct terminal *term)
{
    if (term->cursor.point.row == term->scroll_region.start)
        term_scroll_reverse(term, 1);
    else
        term_cursor_up(term, 1);
}

void
term_reset_view(struct terminal *term)
{
    if (term->grid->view == term->grid->offset)
        return;

    term->grid->view = term->grid->offset;
    term_damage_view(term);
}

void
term_restore_cursor(struct terminal *term)
{
    int row = min(term->saved_cursor.point.row, term->rows - 1);
    int col = min(term->saved_cursor.point.col, term->cols - 1);
    term_cursor_to(term, row, col);
    term->cursor.lcf = term->saved_cursor.lcf;
}

void
term_focus_in(struct terminal *term)
{
    if (!term->focus_events)
        return;
    term_to_slave(term, "\033[I", 3);
}

void
term_focus_out(struct terminal *term)
{
    if (!term->focus_events)
        return;
    term_to_slave(term, "\033[O", 3);
}

static int
linux_mouse_button_to_x(int button)
{
    switch (button) {
    case BTN_LEFT:    return 1;
    case BTN_MIDDLE:  return 2;
    case BTN_RIGHT:   return 3;
    case BTN_BACK:    return 4;
    case BTN_FORWARD: return 5;
    case BTN_SIDE:    return 8;
    case BTN_EXTRA:   return 9;
    case BTN_TASK:    return -1;  /* TODO: ??? */

    default:
        LOG_WARN("unrecognized mouse button: %d (0x%x)", button, button);
        return -1;
    }
}

static int
encode_xbutton(int xbutton)
{
    switch (xbutton) {
    case 1: case 2: case 3:
        return xbutton - 1;

    case 4: case 5:
        /* Like button 1 and 2, but with 64 added */
        return xbutton - 4 + 64;

    case 6: case 7:
        /* Same as 4 and 5. Note: the offset should be something else? */
        return xbutton - 6 + 64;

    case 8: case 9: case 10: case 11:
        /* Similar to 4 and 5, but adding 128 instead of 64 */
        return xbutton - 8 + 128;

    default:
        LOG_ERR("cannot encode X mouse button: %d", xbutton);
        return -1;
    }
}

static void
report_mouse_click(struct terminal *term, int encoded_button, int row, int col,
                   bool release)
{
    char response[128];

    switch (term->mouse_reporting) {
    case MOUSE_NORMAL: {
        int encoded_col = 32 + col + 1;
        int encoded_row = 32 + row + 1;
        if (encoded_col > 255 || encoded_row > 255)
            return;

        snprintf(response, sizeof(response), "\033[M%c%c%c",
                 32 + (release ? 3 : encoded_button), encoded_col, encoded_row);
        break;
    }

    case MOUSE_SGR:
        snprintf(response, sizeof(response), "\033[<%d;%d;%d%c",
                 encoded_button, col + 1, row + 1, release ? 'm' : 'M');
        break;

    case MOUSE_URXVT:
        snprintf(response, sizeof(response), "\033[%d;%d;%dM",
                 32 + (release ? 3 : encoded_button), col + 1, row + 1);
        break;

    case MOUSE_UTF8:
        /* Unimplemented */
        return;
    }

    term_to_slave(term, response, strlen(response));
}

static void
report_mouse_motion(struct terminal *term, int encoded_button, int row, int col)
{
    report_mouse_click(term, encoded_button, row, col, false);
}

void
term_mouse_down(struct terminal *term, int button, int row, int col,
                bool shift, bool alt, bool ctrl)
{
    if (term->wl->kbd.shift) {
        /* "raw" mouse mode */
        return;
    }

    /* Map libevent button event code to X button number */
    int xbutton = linux_mouse_button_to_x(button);
    if (xbutton == -1)
        return;

    int encoded = encode_xbutton(xbutton);
    if (encoded == -1)
        return;

    encoded += (shift ? 4 : 0) + (alt ? 8 : 0) + (ctrl ? 16 : 0);

    switch (term->mouse_tracking) {
    case MOUSE_NONE:
        break;

    case MOUSE_CLICK:
    case MOUSE_DRAG:
    case MOUSE_MOTION:
        report_mouse_click(term, encoded, row, col, false);
        break;

    case MOUSE_X10:
        /* Never enabled */
        assert(false && "unimplemented");
        break;
    }
}

void
term_mouse_up(struct terminal *term, int button, int row, int col,
              bool shift, bool alt, bool ctrl)
{
    if (term->wl->kbd.shift) {
        /* "raw" mouse mode */
        return;
    }

    /* Map libevent button event code to X button number */
    int xbutton = linux_mouse_button_to_x(button);
    if (xbutton == -1)
        return;

    if (xbutton == 4 || xbutton == 5) {
        /* No release events for scroll buttons */
        return;
    }

    int encoded = encode_xbutton(xbutton);
    if (encoded == -1)
        return;

    encoded += (shift ? 4 : 0) + (alt ? 8 : 0) + (ctrl ? 16 : 0);

    switch (term->mouse_tracking) {
    case MOUSE_NONE:
        break;

    case MOUSE_CLICK:
    case MOUSE_DRAG:
    case MOUSE_MOTION:
        report_mouse_click(term, encoded, row, col, true);
        break;

    case MOUSE_X10:
        /* Never enabled */
        assert(false && "unimplemented");
        break;
    }
}

void
term_mouse_motion(struct terminal *term, int button, int row, int col,
                  bool shift, bool alt, bool ctrl)
{
    if (term->wl->kbd.shift) {
        /* "raw" mouse mode */
        return;
    }

    int encoded = 0;

    if (button != 0) {
        /* Map libevent button event code to X button number */
        int xbutton = linux_mouse_button_to_x(button);
        if (xbutton == -1)
            return;

        encoded = encode_xbutton(xbutton);
        if (encoded == -1)
            return;
    } else
        encoded = 3;  /* "released" */

    encoded += 32; /* Motion event */
    encoded += (shift ? 4 : 0) + (alt ? 8 : 0) + (ctrl ? 16 : 0);

    switch (term->mouse_tracking) {
    case MOUSE_NONE:
    case MOUSE_CLICK:
        return;

    case MOUSE_DRAG:
        if (button == 0)
            return;
        /* FALLTHROUGH */

    case MOUSE_MOTION:
        report_mouse_motion(term, encoded, row, col);
        break;

    case MOUSE_X10:
        /* Never enabled */
        assert(false && "unimplemented");
        break;
    }
}

void
term_set_window_title(struct terminal *term, const char *title)
{
    free(term->window_title);
    term->window_title = strdup(title);
    render_set_title(term, term->window_title);
}

void
term_flash(struct terminal *term, unsigned duration_ms)
{
    LOG_DBG("FLASH for %ums", duration_ms);

    struct itimerspec alarm = {
        .it_value = {.tv_sec = 0, .tv_nsec = duration_ms * 1000000},
    };

    if (timerfd_settime(term->flash.fd, 0, &alarm, NULL) < 0)
        LOG_ERRNO("failed to arm flash timer");
    else {
        term->flash.active = true;
    }
}
