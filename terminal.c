#include "terminal.h"

#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <xdg-shell.h>

#define LOG_MODULE "terminal"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "async.h"
#include "commands.h"
#include "config.h"
#include "debug.h"
#include "extract.h"
#include "grid.h"
#include "ime.h"
#include "input.h"
#include "notify.h"
#include "quirks.h"
#include "reaper.h"
#include "render.h"
#include "selection.h"
#include "shm.h"
#include "sixel.h"
#include "slave.h"
#include "spawn.h"
#include "url-mode.h"
#include "util.h"
#include "vt.h"
#include "xmalloc.h"

#define PTMX_TIMING 0

const char *const XCURSOR_HIDDEN = "hidden";
const char *const XCURSOR_LEFT_PTR = "left_ptr";
const char *const XCURSOR_TEXT = "text";
const char *const XCURSOR_TEXT_FALLBACK = "xterm";
//const char *const XCURSOR_HAND2 = "hand2";
const char *const XCURSOR_TOP_LEFT_CORNER = "top_left_corner";
const char *const XCURSOR_TOP_RIGHT_CORNER = "top_right_corner";
const char *const XCURSOR_BOTTOM_LEFT_CORNER = "bottom_left_corner";
const char *const XCURSOR_BOTTOM_RIGHT_CORNER = "bottom_right_corner";
const char *const XCURSOR_LEFT_SIDE = "left_side";
const char *const XCURSOR_RIGHT_SIDE = "right_side";
const char *const XCURSOR_TOP_SIDE = "top_side";
const char *const XCURSOR_BOTTOM_SIDE = "bottom_side";

static void
enqueue_data_for_slave(const void *data, size_t len, size_t offset,
                       ptmx_buffer_list_t *buffer_list)
{
    void *copy = xmalloc(len);
    memcpy(copy, data, len);

    struct ptmx_buffer queued = {
        .data = copy,
        .len = len,
        .idx = offset,
    };
    tll_push_back(*buffer_list, queued);
}

static bool
data_to_slave(struct terminal *term, const void *data, size_t len,
              ptmx_buffer_list_t *buffer_list)
{
    /*
     * Try a synchronous write first. If we fail to write everything,
     * switch to asynchronous.
     */

    size_t async_idx = 0;
    switch (async_write(term->ptmx, data, len, &async_idx)) {
    case ASYNC_WRITE_REMAIN:
        /* Switch to asynchronous mode; let FDM write the remaining data */
        if (!fdm_event_add(term->fdm, term->ptmx, EPOLLOUT))
            return false;
        enqueue_data_for_slave(data, len, async_idx, buffer_list);
        return true;

    case ASYNC_WRITE_DONE:
        return true;

    case ASYNC_WRITE_ERR:
        LOG_ERRNO("failed to synchronously write %zu bytes to slave", len);
        return false;
    }

    BUG("Unexpected async_write() return value");
    return false;
}

bool
term_paste_data_to_slave(struct terminal *term, const void *data, size_t len)
{
    xassert(term->is_sending_paste_data);

    if (term->ptmx < 0) {
        /* We're probably in "hold" */
        return false;
    }

    if (tll_length(term->ptmx_paste_buffers) > 0) {
        /* Don't even try to send data *now* if there's queued up
         * data, since that would result in events arriving out of
         * order. */
        enqueue_data_for_slave(data, len, 0, &term->ptmx_paste_buffers);
        return true;
    }

    return data_to_slave(term, data, len, &term->ptmx_paste_buffers);
}

bool
term_to_slave(struct terminal *term, const void *data, size_t len)
{
    if (term->ptmx < 0) {
        /* We're probably in "hold" */
        return false;
    }

    if (tll_length(term->ptmx_buffers) > 0 || term->is_sending_paste_data) {
        /*
         * Don't even try to send data *now* if there's queued up
         * data, since that would result in events arriving out of
         * order.
         *
         * Furthermore, if we're currently sending paste data to the
         * client, do *not* mix that stream with other events
         * (https://codeberg.org/dnkl/foot/issues/101).
         */
        enqueue_data_for_slave(data, len, 0, &term->ptmx_buffers);
        return true;
    }

    return data_to_slave(term, data, len, &term->ptmx_buffers);
}

static bool
fdm_ptmx_out(struct fdm *fdm, int fd, int events, void *data)
{
    struct terminal *term = data;

    /* If there is no queued data, then we shouldn't be in asynchronous mode */
    xassert(tll_length(term->ptmx_buffers) > 0 ||
           tll_length(term->ptmx_paste_buffers) > 0);

    /* Writes a single buffer, returns if not all of it could be written */
#define write_one_buffer(buffer_list)                                   \
    {                                                                   \
        switch (async_write(term->ptmx, it->item.data, it->item.len, &it->item.idx)) { \
        case ASYNC_WRITE_DONE:                                          \
            free(it->item.data);                                        \
            tll_remove(buffer_list, it);                                \
            break;                                                      \
        case ASYNC_WRITE_REMAIN:                                        \
            /* to_slave() updated it->item.idx */                       \
            return true;                                                \
        case ASYNC_WRITE_ERR:                                           \
            LOG_ERRNO("failed to asynchronously write %zu bytes to slave", \
                      it->item.len - it->item.idx);                     \
            return false;                                               \
        }                                                               \
    }

    tll_foreach(term->ptmx_paste_buffers, it)
        write_one_buffer(term->ptmx_paste_buffers);

    /* If we get here, *all* paste data buffers were successfully
     * flushed */

    if (!term->is_sending_paste_data) {
        tll_foreach(term->ptmx_buffers, it)
            write_one_buffer(term->ptmx_buffers);
    }

    /*
     * If we get here, *all* buffers were successfully flushed.
     *
     * Or, we're still sending paste data, in which case we do *not*
     * want to send the "normal" queued up data
     *
     * In both cases, we want to *disable* the FDM callback since
     * otherwise we'd just be called right away again, with nothing to
     * write.
     */
    fdm_event_del(term->fdm, term->ptmx, EPOLLOUT);
    return true;
}

#if PTMX_TIMING
static struct timespec last = {0};
#endif

static bool cursor_blink_rearm_timer(struct terminal *term);

/* Externally visible, but not declared in terminal.h, to enable pgo
 * to call this function directly */
bool
fdm_ptmx(struct fdm *fdm, int fd, int events, void *data)
{
    struct terminal *term = data;

    const bool pollin = events & EPOLLIN;
    const bool pollout = events & EPOLLOUT;
    const bool hup = events & EPOLLHUP;

    if (pollout) {
        if (!fdm_ptmx_out(fdm, fd, events, data))
            return false;
    }

    /* Prevent blinking while typing */
    if (term->cursor_blink.fd >= 0) {
        term->cursor_blink.state = CURSOR_BLINK_ON;
        cursor_blink_rearm_timer(term);
    }

    uint8_t buf[24 * 1024];
    const size_t max_iterations = !hup ? 10 : (size_t)-1ll;

    for (size_t i = 0; i < max_iterations && pollin; i++) {
        xassert(pollin);
        ssize_t count = read(term->ptmx, buf, sizeof(buf));

        if (count < 0) {
            if (errno == EAGAIN || errno == EIO) {
                /*
                 * EAGAIN: no more to read - FDM will trigger us again
                 * EIO: assume PTY was closed - we already have, or will get, a EPOLLHUP
                 */
                break;
            }

            LOG_ERRNO("failed to read from pseudo terminal");
            return false;
        } else if (count == 0) {
            /* Reached end-of-file */
            break;
        }

        vt_from_slave(term, buf, count);
    }

    if (!term->render.app_sync_updates.enabled) {
        /*
         * We likely need to re-render. But, we don't want to do it
         * immediately. Often, a single client update is done through
         * multiple writes. This could lead to us rendering one frame with
         * "intermediate" state.
         *
         * For example, we might end up rendering a frame
         * where the client just erased a line, while in the
         * next frame, the client wrote to the same line. This
         * causes screen "flickering".
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
         */
        uint64_t lower_ns = term->conf->tweak.delayed_render_lower_ns;
        uint64_t upper_ns = term->conf->tweak.delayed_render_upper_ns;

        if (lower_ns > 0 && upper_ns > 0) {
#if PTMX_TIMING
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);
            if (last.tv_sec > 0 || last.tv_nsec > 0) {
                struct timespec diff;

                timespec_sub(&now, &last, &diff);
                LOG_INFO("waited %lds %ldns for more input",
                         (long)diff.tv_sec, diff.tv_nsec);
            }
            last = now;
#endif

            xassert(lower_ns < 1000000000);
            xassert(upper_ns < 1000000000);
            xassert(upper_ns > lower_ns);

            timerfd_settime(
                term->delayed_render_timer.lower_fd, 0,
                &(struct itimerspec){.it_value = {.tv_nsec = lower_ns}},
                NULL);

            /* Second timeout - only reset when we render. Set to one
             * frame (assuming 60Hz) */
            if (!term->delayed_render_timer.is_armed) {
                timerfd_settime(
                    term->delayed_render_timer.upper_fd, 0,
                    &(struct itimerspec){.it_value = {.tv_nsec = upper_ns}},
                    NULL);
                term->delayed_render_timer.is_armed = true;
            }
        } else
            render_refresh(term);
    }

    if (hup) {
        fdm_del(fdm, fd);
        term->ptmx = -1;
    }

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
    render_refresh(term);

    /* Work around Sway bug - unmapping a sub-surface does not damage
     * the underlying surface */
    term_damage_margins(term);
    term_damage_view(term);
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
    bool no_blinking_cells = true;
    for (int r = 0; r < term->rows; r++) {
        struct row *row = grid_row_in_view(term->grid, r);
        for (int col = 0; col < term->cols; col++) {
            struct cell *cell = &row->cells[col];

            if (cell->attrs.blink) {
                cell->attrs.clean = 0;
                row->dirty = true;
                no_blinking_cells = false;
            }
        }
    }

    if (no_blinking_cells) {
        LOG_DBG("disarming blink timer");

        term->blink.state = BLINK_ON;
        fdm_del(term->fdm, term->blink.fd);
        term->blink.fd = -1;
    } else
        render_refresh(term);
    return true;
}

void
term_arm_blink_timer(struct terminal *term)
{
    if (term->blink.fd >= 0)
        return;

    LOG_DBG("arming blink timer");

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0) {
        LOG_ERRNO("failed to create blink timer FD");
        return;
    }

    if (!fdm_add(term->fdm, fd, EPOLLIN, &fdm_blink, term)) {
        close(fd);
        return;
    }

    struct itimerspec alarm = {
        .it_value = {.tv_sec = 0, .tv_nsec = 500 * 1000000},
        .it_interval = {.tv_sec = 0, .tv_nsec = 500 * 1000000},
    };

    if (timerfd_settime(fd, 0, &alarm, NULL) < 0) {
        LOG_ERRNO("failed to arm blink timer");
        fdm_del(term->fdm, fd);
    }

    term->blink.fd = fd;
}

static void
cursor_refresh(struct terminal *term)
{
    term->grid->cur_row->cells[term->grid->cursor.point.col].attrs.clean = 0;
    term->grid->cur_row->dirty = true;
    render_refresh(term);
}

static bool
fdm_cursor_blink(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct terminal *term = data;
    uint64_t expiration_count;
    ssize_t ret = read(
        term->cursor_blink.fd, &expiration_count, sizeof(expiration_count));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read cursor blink timer");
        return false;
    }

    LOG_DBG("cursor blink timer expired %llu times",
            (unsigned long long)expiration_count);

    /* Invert blink state */
    term->cursor_blink.state = term->cursor_blink.state == CURSOR_BLINK_ON
        ? CURSOR_BLINK_OFF : CURSOR_BLINK_ON;

    cursor_refresh(term);
    return true;
}

static bool
fdm_delayed_render(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct terminal *term = data;

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

    if (ret1 > 0)
        LOG_DBG("lower delay timer expired");
    else if (ret2 > 0)
        LOG_DBG("upper delay timer expired");

    if (ret1 == 0 && ret2 == 0)
        return true;

#if PTMX_TIMING
    last = (struct timespec){0};
#endif

    /* Reset timers */
    struct itimerspec reset = {{0}};
    timerfd_settime(term->delayed_render_timer.lower_fd, 0, &reset, NULL);
    timerfd_settime(term->delayed_render_timer.upper_fd, 0, &reset, NULL);
    term->delayed_render_timer.is_armed = false;

    render_refresh(term);
    return true;
}

static bool
fdm_app_sync_updates_timeout(
    struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct terminal *term = data;
    uint64_t unused;
    ssize_t ret = read(term->render.app_sync_updates.timer_fd,
                       &unused, sizeof(unused));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;
        LOG_ERRNO("failed to read application synchronized updates timeout timer");
        return false;
    }

    term_disable_app_sync_updates(term);
    return true;
}

static bool
fdm_title_update_timeout(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct terminal *term = data;
    uint64_t unused;
    ssize_t ret = read(term->render.title.timer_fd, &unused, sizeof(unused));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;
        LOG_ERRNO("failed to read title update throttle timer");
        return false;
    }

    struct itimerspec reset = {{0}};
    timerfd_settime(term->render.title.timer_fd, 0, &reset, NULL);
    term->render.title.is_armed = false;

    render_refresh_title(term);
    return true;
}

static bool
initialize_render_workers(struct terminal *term)
{
    LOG_INFO("using %hu rendering threads", term->render.workers.count);

    if (sem_init(&term->render.workers.start, 0, 0) < 0 ||
        sem_init(&term->render.workers.done, 0, 0) < 0)
    {
        LOG_ERRNO("failed to instantiate render worker semaphores");
        return false;
    }

    int err;
    if ((err = mtx_init(&term->render.workers.lock, mtx_plain)) != thrd_success) {
        LOG_ERR("failed to instantiate render worker mutex: %s (%d)",
                thrd_err_as_string(err), err);
        goto err_sem_destroy;
    }

    term->render.workers.threads = xcalloc(
        term->render.workers.count, sizeof(term->render.workers.threads[0]));

    for (size_t i = 0; i < term->render.workers.count; i++) {
        struct render_worker_context *ctx = xmalloc(sizeof(*ctx));
        *ctx = (struct render_worker_context) {
            .term = term,
            .my_id = 1 + i,
        };

        int ret = thrd_create(
            &term->render.workers.threads[i], &render_worker_thread, ctx);
        if (ret != thrd_success) {

            LOG_ERR("failed to create render worker thread: %s (%d)",
                    thrd_err_as_string(ret), ret);
            term->render.workers.threads[i] = 0;
            return false;
        }
    }

    return true;

err_sem_destroy:
    sem_destroy(&term->render.workers.start);
    sem_destroy(&term->render.workers.done);
    return false;
}

static void
free_custom_glyph(struct fcft_glyph **glyph)
{
    if (*glyph == NULL)
        return;

    free(pixman_image_get_data((*glyph)->pix));
    pixman_image_unref((*glyph)->pix);
    free(*glyph);
    *glyph = NULL;
}

static void
free_custom_glyphs(struct fcft_glyph ***glyphs, size_t count)
{
    if (*glyphs == NULL)
        return;

    for (size_t i = 0; i < count; i++)
        free_custom_glyph(&(*glyphs)[i]);

    free(*glyphs);
    *glyphs = NULL;
}

static bool
term_set_fonts(struct terminal *term, struct fcft_font *fonts[static 4])
{
    for (size_t i = 0; i < 4; i++) {
        xassert(fonts[i] != NULL);

        fcft_destroy(term->fonts[i]);
        term->fonts[i] = fonts[i];
    }

    free_custom_glyphs(
        &term->custom_glyphs.box_drawing, GLYPH_BOX_DRAWING_COUNT);
    free_custom_glyphs(
        &term->custom_glyphs.braille, GLYPH_BRAILLE_COUNT);
    free_custom_glyphs(
        &term->custom_glyphs.legacy, GLYPH_LEGACY_COUNT);

    const int old_cell_width = term->cell_width;
    const int old_cell_height = term->cell_height;

    const struct config *conf = term->conf;

    const struct fcft_glyph *M = fcft_rasterize_char_utf32(
        fonts[0], U'M', term->font_subpixel);
    int advance = M != NULL ? M->advance.x : term->fonts[0]->max_advance.x;

    term->cell_width = advance +
        term_pt_or_px_as_pixels(term, &conf->letter_spacing);

    term->cell_height = term->font_line_height.px >= 0
        ? term_pt_or_px_as_pixels(term, &term->font_line_height)
        : max(term->fonts[0]->height,
              term->fonts[0]->ascent + term->fonts[0]->descent);

    if (term->cell_width <= 0)
        term->cell_width = 1;
    if (term->cell_height <= 0)
        term->cell_height = 1;

    term->font_x_ofs = term_pt_or_px_as_pixels(term, &conf->horizontal_letter_offset);
    term->font_y_ofs = term_pt_or_px_as_pixels(term, &conf->vertical_letter_offset);

    LOG_INFO("cell width=%d, height=%d", term->cell_width, term->cell_height);

    if (term->cell_width < old_cell_width ||
        term->cell_height < old_cell_height)
    {
        /*
         * The cell size has decreased.
         *
         * This means sixels, which we cannot resize, no longer fit
         * into their "allocated" grid space.
         *
         * To be able to fit them, we would have to change the grid
         * content. Inserting empty lines _might_ seem acceptable, but
         * we'd also need to insert empty columns, which would break
         * existing layout completely.
         *
         * So we delete them.
         */
        sixel_destroy_all(term);
    } else if (term->cell_width != old_cell_width ||
               term->cell_height != old_cell_height)
    {
        sixel_cell_size_changed(term);
    }

    /* Use force, since cell-width/height may have changed */
    render_resize_force(term, term->width / term->scale, term->height / term->scale);
    return true;
}

static float
get_font_dpi(const struct terminal *term)
{
    /*
     * Use output's DPI to scale font. This is to ensure the font has
     * the same physical height (if measured by a ruler) regardless of
     * monitor.
     *
     * Conceptually, we use the physical monitor specs to calculate
     * the DPI, and we ignore the output's scaling factor.
     *
     * However, to deal with fractional scaling, where we're told to
     * render at e.g. 2x, but are then downscaled by the compositor to
     * e.g. 1.25, we use the scaled DPI value multiplied by the scale
     * factor instead.
     *
     * For integral scaling factors the resulting DPI is the same as
     * if we had used the physical DPI.
     *
     * For fractional scaling factors we'll get a DPI *larger* than
     * the physical DPI, that ends up being right when later
     * downscaled by the compositor.
     */

    /* Use highest DPI from outputs we're mapped on */
    double dpi = 0.0;
    xassert(term->window != NULL);
    tll_foreach(term->window->on_outputs, it) {
        if (it->item->dpi > dpi)
            dpi = it->item->dpi;
    }

    /* If we're not mapped, use DPI from first monitor. Hopefully this is where we'll get mapped later... */
    if (dpi == 0.) {
        tll_foreach(term->wl->monitors, it) {
            dpi = it->item.dpi;
            break;
        }
    }

    if (dpi == 0) {
        /* No monitors? */
        dpi = 96.;
    }

    return dpi;
}

static enum fcft_subpixel
get_font_subpixel(const struct terminal *term)
{
    if (term->colors.alpha != 0xffff) {
        /* Can't do subpixel rendering on transparent background */
        return FCFT_SUBPIXEL_NONE;
    }

    enum wl_output_subpixel wl_subpixel;

    /*
     * Wayland doesn't tell us *which* part of the surface that goes
     * on a specific output, only whether the surface is mapped to an
     * output or not.
     *
     * Thus, when determining which subpixel mode to use, we can't do
     * much but select *an* output. So, we pick the first one.
     *
     * If we're not mapped at all, we pick the first available
     * monitor, and hope that's where we'll eventually get mapped.
     *
     * If there aren't any monitors we use the "default" subpixel
     * mode.
     */

    if (tll_length(term->window->on_outputs) > 0)
        wl_subpixel = tll_front(term->window->on_outputs)->subpixel;
    else if (tll_length(term->wl->monitors) > 0)
        wl_subpixel = tll_front(term->wl->monitors).subpixel;
    else
        wl_subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;

    switch (wl_subpixel) {
    case WL_OUTPUT_SUBPIXEL_UNKNOWN:        return FCFT_SUBPIXEL_DEFAULT;
    case WL_OUTPUT_SUBPIXEL_NONE:           return FCFT_SUBPIXEL_NONE;
    case WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB: return FCFT_SUBPIXEL_HORIZONTAL_RGB;
    case WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR: return FCFT_SUBPIXEL_HORIZONTAL_BGR;
    case WL_OUTPUT_SUBPIXEL_VERTICAL_RGB:   return FCFT_SUBPIXEL_VERTICAL_RGB;
    case WL_OUTPUT_SUBPIXEL_VERTICAL_BGR:   return FCFT_SUBPIXEL_VERTICAL_BGR;
    }

    return FCFT_SUBPIXEL_DEFAULT;
}

static bool
term_font_size_by_dpi(const struct terminal *term)
{
    switch (term->conf->dpi_aware) {
    case DPI_AWARE_YES:  return true;
    case DPI_AWARE_NO:   return false;

    case DPI_AWARE_AUTO:
        /*
         * Scale using DPI if all monitors have a scaling factor or 1.
         *
         * The idea is this: if a user, with multiple monitors, have
         * enabled scaling on at least one monitor, then he/she has
         * most likely done so to match the size of his/hers other
         * monitors.
         *
         * I.e. if the user has one monitor with a scaling factor of
         * one, and another with a scaling factor of two, he/she
         * expects things to be twice as large on the second
         * monitor.
         *
         * If we (foot) scale using DPI on the first monitor, and
         * using the scaling factor on the second monitor, foot will
         * *not* look twice as big on the second monitor.
         */
        tll_foreach(term->wl->monitors, it) {
            const struct monitor *mon = &it->item;
            if (mon->scale > 1)
                return false;
        }
        return true;
    }

    BUG("unhandled DPI awareness value");
}

int
term_pt_or_px_as_pixels(const struct terminal *term,
                        const struct pt_or_px *pt_or_px)
{
    double scale = !term->font_is_sized_by_dpi ? term->scale : 1.;
    double dpi = term->font_is_sized_by_dpi  ? term->font_dpi : 96.;

    return pt_or_px->px == 0
        ? round(pt_or_px->pt * scale * dpi / 72)
        : pt_or_px->px;
}

struct font_load_data {
    size_t count;
    const char **names;
    const char *attrs;

    struct fcft_font **font;
};

static int
font_loader_thread(void *_data)
{
    struct font_load_data *data = _data;
    *data->font = fcft_from_name(data->count, data->names, data->attrs);
    return *data->font != NULL;
}

static bool
reload_fonts(struct terminal *term)
{
    const struct config *conf = term->conf;

    const size_t counts[4] = {
        conf->fonts[0].count,
        conf->fonts[1].count,
        conf->fonts[2].count,
        conf->fonts[3].count,
    };

    /* Configure size (which may have been changed run-time) */
    char **names[4];
    for (size_t i = 0; i < 4; i++) {
        names[i] = xmalloc(counts[i] * sizeof(names[i][0]));

        const struct config_font_list *font_list = &conf->fonts[i];

        for (size_t j = 0; j < font_list->count; j++) {
            const struct config_font *font = &font_list->arr[j];
            bool use_px_size = term->font_sizes[i][j].px_size > 0;
            char size[64];

            const int scale = term->font_is_sized_by_dpi ? 1 : term->scale;

            if (use_px_size)
                snprintf(size, sizeof(size), ":pixelsize=%d",
                         term->font_sizes[i][j].px_size * scale);
            else
                snprintf(size, sizeof(size), ":size=%.2f",
                         term->font_sizes[i][j].pt_size * (double)scale);

            size_t len = strlen(font->pattern) + strlen(size) + 1;
            names[i][j] = xmalloc(len);

            strcpy(names[i][j], font->pattern);
            strcat(names[i][j], size);
        }
    }

    /* Did user configure custom bold/italic fonts?
     * Or should we use the regular font, with weight/slant attributes? */
    const bool custom_bold = counts[1] > 0;
    const bool custom_italic = counts[2] > 0;
    const bool custom_bold_italic = counts[3] > 0;

    const size_t count_regular = counts[0];
    const char **names_regular = (const char **)names[0];

    const size_t count_bold = custom_bold ? counts[1] : counts[0];
    const char **names_bold = (const char **)(custom_bold ? names[1] : names[0]);

    const size_t count_italic = custom_italic ? counts[2] : counts[0];
    const char **names_italic = (const char **)(custom_italic ? names[2] : names[0]);

    const size_t count_bold_italic = custom_bold_italic ? counts[3] : counts[0];
    const char **names_bold_italic = (const char **)(custom_bold_italic ? names[3] : names[0]);

    const bool use_dpi = term->font_is_sized_by_dpi;

    char *attrs[4] = {NULL};
    int attr_len[4] = {-1, -1, -1, -1};  /* -1, so that +1 (below) results in 0 */

    for (size_t i = 0; i < 2; i++) {
        attr_len[0] = snprintf(
            attrs[0], attr_len[0] + 1, "dpi=%.2f",
            use_dpi ? term->font_dpi : 96);
        attr_len[1] = snprintf(
            attrs[1], attr_len[1] + 1, "dpi=%.2f:%s",
            use_dpi ? term->font_dpi : 96, !custom_bold ? "weight=bold" : "");
        attr_len[2] = snprintf(
            attrs[2], attr_len[2] + 1, "dpi=%.2f:%s",
            use_dpi ? term->font_dpi : 96, !custom_italic ? "slant=italic" : "");
        attr_len[3] = snprintf(
            attrs[3], attr_len[3] + 1, "dpi=%.2f:%s",
            use_dpi ? term->font_dpi : 96, !custom_bold_italic ? "weight=bold:slant=italic" : "");

        if (i > 0)
            continue;

        for (size_t i = 0; i < 4; i++)
            attrs[i] = xmalloc(attr_len[i] + 1);
    }

    struct fcft_font *fonts[4];
    struct font_load_data data[4] = {
        {count_regular,     names_regular,     attrs[0], &fonts[0]},
        {count_bold,        names_bold,        attrs[1], &fonts[1]},
        {count_italic,      names_italic,      attrs[2], &fonts[2]},
        {count_bold_italic, names_bold_italic, attrs[3], &fonts[3]},
    };

    thrd_t tids[4] = {0};
    for (size_t i = 0; i < 4; i++) {
        int ret = thrd_create(&tids[i], &font_loader_thread, &data[i]);
        if (ret != thrd_success) {
            LOG_ERR("failed to create font loader thread: %s (%d)",
                    thrd_err_as_string(ret), ret);
            break;
        }
    }

    bool success = true;
    for (size_t i = 0; i < 4; i++) {
        if (tids[i] != 0) {
            int ret;
            thrd_join(tids[i], &ret);
            success = success && ret;
        } else
            success = false;
    }

    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < counts[i]; j++)
            free(names[i][j]);
        free(names[i]);
        free(attrs[i]);
    }

    if (!success) {
        LOG_ERR("failed to load primary fonts");
        for (size_t i = 0; i < 4; i++) {
            fcft_destroy(fonts[i]);
            fonts[i] = NULL;
        }
    }

    return success ? term_set_fonts(term, fonts) : success;
}

static bool
load_fonts_from_conf(struct terminal *term)
{
    const struct config *conf = term->conf;

    for (size_t i = 0; i < 4; i++) {
        const struct config_font_list *font_list = &conf->fonts[i];

        for (size_t j = 0; j < font_list->count; j++) {
            const struct config_font *font = &font_list->arr[j];
            term->font_sizes[i][j] = (struct config_font){
                .pt_size = font->pt_size, .px_size = font->px_size};
        }
    }

    term->font_line_height = term->conf->line_height;
    return reload_fonts(term);
}

static void fdm_client_terminated(
    struct reaper *reaper, pid_t pid, int status, void *data);

struct terminal *
term_init(const struct config *conf, struct fdm *fdm, struct reaper *reaper,
          struct wayland *wayl, const char *foot_exe, const char *cwd,
          const char *token, int argc, char *const *argv, char *const *envp,
          void (*shutdown_cb)(void *data, int exit_code), void *shutdown_data)
{
    int ptmx = -1;
    int flash_fd = -1;
    int delay_lower_fd = -1;
    int delay_upper_fd = -1;
    int app_sync_updates_fd = -1;
    int title_update_fd = -1;

    struct terminal *term = malloc(sizeof(*term));
    if (unlikely(term == NULL)) {
        LOG_ERRNO("malloc() failed");
        return NULL;
    }

    if ((ptmx = posix_openpt(O_RDWR | O_NOCTTY)) < 0) {
        LOG_ERRNO("failed to open PTY");
        goto close_fds;
    }
    if ((flash_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)) < 0) {
        LOG_ERRNO("failed to create flash timer FD");
        goto close_fds;
    }
    if ((delay_lower_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)) < 0 ||
        (delay_upper_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)) < 0)
    {
        LOG_ERRNO("failed to create delayed rendering timer FDs");
        goto close_fds;
    }

    if ((app_sync_updates_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)) < 0)
    {
        LOG_ERRNO("failed to create application synchronized updates timer FD");
        goto close_fds;
    }

    if ((title_update_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)) < 0)
    {
        LOG_ERRNO("failed to create title update throttle timer FD");
        goto close_fds;
    }

    if (ioctl(ptmx, (unsigned int)TIOCSWINSZ,
              &(struct winsize){.ws_row = 24, .ws_col = 80}) < 0)
    {
        LOG_ERRNO("failed to set initial TIOCSWINSZ");
        goto close_fds;
    }

    int ptmx_flags;
    if ((ptmx_flags = fcntl(ptmx, F_GETFL)) < 0 ||
        fcntl(ptmx, F_SETFL, ptmx_flags | O_NONBLOCK) < 0)
    {
        LOG_ERRNO("failed to configure ptmx as non-blocking");
        goto err;
    }

    /*
     * Enable all FDM callbackes *except* ptmx - we can't do that
     * until the window has been 'configured' since we don't have a
     * size (and thus no grid) before then.
     */

    if (!fdm_add(fdm, flash_fd, EPOLLIN, &fdm_flash, term) ||
        !fdm_add(fdm, delay_lower_fd, EPOLLIN, &fdm_delayed_render, term) ||
        !fdm_add(fdm, delay_upper_fd, EPOLLIN, &fdm_delayed_render, term) ||
        !fdm_add(fdm, app_sync_updates_fd, EPOLLIN, &fdm_app_sync_updates_timeout, term) ||
        !fdm_add(fdm, title_update_fd, EPOLLIN, &fdm_title_update_timeout, term))
    {
        goto err;
    }

    /* Initialize configure-based terminal attributes */
    *term = (struct terminal) {
        .fdm = fdm,
        .reaper = reaper,
        .conf = conf,
        .ptmx = ptmx,
        .ptmx_buffers = tll_init(),
        .ptmx_paste_buffers = tll_init(),
        .font_sizes = {
            xmalloc(sizeof(term->font_sizes[0][0]) * conf->fonts[0].count),
            xmalloc(sizeof(term->font_sizes[1][0]) * conf->fonts[1].count),
            xmalloc(sizeof(term->font_sizes[2][0]) * conf->fonts[2].count),
            xmalloc(sizeof(term->font_sizes[3][0]) * conf->fonts[3].count),
        },
        .font_dpi = 0.,
        .font_subpixel = (conf->colors.alpha == 0xffff  /* Can't do subpixel rendering on transparent background */
                          ? FCFT_SUBPIXEL_DEFAULT
                          : FCFT_SUBPIXEL_NONE),
        .cursor_keys_mode = CURSOR_KEYS_NORMAL,
        .keypad_keys_mode = KEYPAD_NUMERICAL,
        .reverse_wrap = true,
        .auto_margin = true,
        .window_title_stack = tll_init(),
        .scale = 1,
        .flash = {.fd = flash_fd},
        .blink = {.fd = -1},
        .vt = {
            .state = 0,  /* STATE_GROUND */
        },
        .colors = {
            .fg = conf->colors.fg,
            .bg = conf->colors.bg,
            .alpha = conf->colors.alpha,
            .selection_fg = conf->colors.selection_fg,
            .selection_bg = conf->colors.selection_bg,
            .use_custom_selection = conf->colors.use_custom.selection,
        },
        .origin = ORIGIN_ABSOLUTE,
        .cursor_style = conf->cursor.style,
        .cursor_blink = {
            .decset = false,
            .deccsusr = conf->cursor.blink,
            .state = CURSOR_BLINK_ON,
            .fd = -1,
        },
        .cursor_color = {
            .text = conf->cursor.color.text,
            .cursor = conf->cursor.color.cursor,
        },
        .selection = {
            .coords = {
                .start = {-1, -1},
                .end = {-1, -1},
            },
            .pivot = {
                .start = {-1, -1},
                .end = {-1, -1},
            },
            .auto_scroll = {
                .fd = -1,
            },
        },
        .normal = {.scroll_damage = tll_init(), .sixel_images = tll_init()},
        .alt = {.scroll_damage = tll_init(), .sixel_images = tll_init()},
        .grid = &term->normal,
        .composed = NULL,
        .alt_scrolling = conf->mouse.alternate_scroll_mode,
        .meta = {
            .esc_prefix = true,
            .eight_bit = true,
        },
        .num_lock_modifier = true,
        .bell_action_enabled = true,
        .tab_stops = tll_init(),
        .wl = wayl,
        .render = {
            .chains = {
                .grid = shm_chain_new(wayl->shm, true, 1 + conf->render_worker_count),
                .search = shm_chain_new(wayl->shm, false, 1),
                .scrollback_indicator = shm_chain_new(wayl->shm, false, 1),
                .render_timer = shm_chain_new(wayl->shm, false, 1),
                .url = shm_chain_new(wayl->shm, false, 1),
                .csd = shm_chain_new(wayl->shm, false, 1),
                .overlay = shm_chain_new(wayl->shm, false, 1),
            },
            .scrollback_lines = conf->scrollback.lines,
            .app_sync_updates.timer_fd = app_sync_updates_fd,
            .title = {
                .is_armed = false,
                .timer_fd = title_update_fd,
            },
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
        .sixel = {
            .scrolling = true,
            .use_private_palette = true,
            .palette_size = SIXEL_MAX_COLORS,
            .max_width = SIXEL_MAX_WIDTH,
            .max_height = SIXEL_MAX_HEIGHT,
        },
        .shutdown = {
            .terminate_timeout_fd = -1,
            .cb = shutdown_cb,
            .cb_data = shutdown_data,
        },
        .foot_exe = xstrdup(foot_exe),
        .cwd = xstrdup(cwd),
#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
        .ime_enabled = true,
#endif
    };

    pixman_region32_init(&term->render.last_overlay_clip);

    term_update_ascii_printer(term);

    for (size_t i = 0; i < 4; i++) {
        const struct config_font_list *font_list = &conf->fonts[i];
        for (size_t j = 0; j < font_list->count; j++) {
            const struct config_font *font = &font_list->arr[j];
            term->font_sizes[i][j] = (struct config_font){
                .pt_size = font->pt_size, .px_size = font->px_size};
        }
    }
    term->font_line_height = conf->line_height;

    /* Start the slave/client */
    if ((term->slave = slave_spawn(
             term->ptmx, argc, term->cwd, argv, envp, &conf->env_vars,
             conf->term, conf->shell, conf->login_shell,
             &conf->notifications)) == -1)
    {
        goto err;
    }

    reaper_add(term->reaper, term->slave, &fdm_client_terminated, term);

    /* Guess scale; we're not mapped yet, so we don't know on which
     * output we'll be. Pick highest scale we find for now */
    tll_foreach(term->wl->monitors, it) {
        if (it->item.scale > term->scale)
            term->scale = it->item.scale;
    }

    memcpy(term->colors.table, term->conf->colors.table, sizeof(term->colors.table));

    key_binding_new_for_term(wayl->key_binding_manager, term);

    /* Initialize the Wayland window backend */
    if ((term->window = wayl_win_init(term, token)) == NULL)
        goto err;

    /* Load fonts */
    if (!term_font_dpi_changed(term, 0))
        goto err;

    term->font_subpixel = get_font_subpixel(term);

    term_set_window_title(term, conf->title);

    /* Let the Wayland backend know we exist */
    tll_push_back(wayl->terms, term);

    switch (conf->startup_mode) {
    case STARTUP_WINDOWED:
        break;

    case STARTUP_MAXIMIZED:
        xdg_toplevel_set_maximized(term->window->xdg_toplevel);
        break;

    case STARTUP_FULLSCREEN:
        xdg_toplevel_set_fullscreen(term->window->xdg_toplevel, NULL);
        break;
    }

    if (!initialize_render_workers(term))
        goto err;

    return term;

err:
    term->shutdown.in_progress = true;
    term_destroy(term);
    return NULL;

close_fds:
    close(ptmx);
    fdm_del(fdm, flash_fd);
    fdm_del(fdm, delay_lower_fd);
    fdm_del(fdm, delay_upper_fd);
    fdm_del(fdm, app_sync_updates_fd);
    fdm_del(fdm, title_update_fd);

    free(term);
    return NULL;
}

void
term_window_configured(struct terminal *term)
{
    /* Enable ptmx FDM callback */
    if (!term->shutdown.in_progress) {
        xassert(term->window->is_configured);
        fdm_add(term->fdm, term->ptmx, EPOLLIN, &fdm_ptmx, term);
    }
}

/*
 * Shutdown logic
 *
 * A foot instance can be terminated in two ways:
 *
 *  - the client application terminates (user types ‘exit’, or pressed C-d in the
 *    shell, etc)
 *  - the foot window is closed
 *
 * Both variants need to trigger to “other” action. I.e. if the client
 * application is terminated, then we need to close the window. If the window is
 * closed, we need to terminate the client application.
 *
 * Only when *both* tasks have completed do we consider ourselves fully
 * shutdown. This is when we can call term_destroy(), and the user provided
 * shutdown callback.
 *
 * The functions involved with this are:
 *
 * - shutdown_maybe_done(): called after any of the two tasks above have
 *   completed. When it determines that *both* tasks are done, it calls
 *   term_destroy() and the user provided shutdown callback.
 *
 * - fdm_client_terminated(): reaper callback, called when the client
 *   application has terminated.
 *
 *     + Kills the “terminate” timeout timer
 *     + Calls shutdown_maybe_done() if the shutdown procedure has already
 *       started (i.e. the window being closed initiated the shutdown)
 *    -OR-
 *       Initiates the shutdown itself, by calling term_shutdown() (client
 *       application termination initiated the shutdown).
 *
 * - term_shutdown(): unregisters all FDM callbacks, sends SIGTERM to the client
 *   application and installs a “terminate” timeout timer (if it hasn’t already
 *   terminated). Finally registers an event FD with the FDM, which is
 *   immediately triggered. This is done to ensure any pending FDM events are
 *   handled before shutting down.
 *
 * - fdm_shutdown(): FDM callback, triggered by the event FD in
 *   term_shutdown(). Unmaps and destroys the window resources, and ensures the
 *   seats’ focused pointers don’t reference us. Finally calls
 *   shutdown_maybe_done().
 *
 * - fdm_terminate_timeout(): FDM callback for the “terminate” timeout
 *   timer. This function is called when the client application hasn’t
 *   terminated after 60 seconds (after the SIGTERM). Sends SIGKILL to the
 *   client application.
 *
 * - term_destroy(): normally called from shutdown_maybe_done(), when both the
 *   window has been unmapped, and the client application has terminated. In
 *   this case, it simply destroys all resources.
 *
 *   It may however also be called without term_shutdown() having been called
 *   (typically in error code paths - for example, when the Wayland connection
 *   is closed by the compositor). In this case, the client application is
 *   typically still running, and we can’t assume the FDM is running. To handle
 *   this, we install configure a 60 second SIGALRM, send SIGTERM to the client
 *   application, and then enter a blocking waitpid().
 *
 *   If the alarm triggers, we send SIGKILL and once again enter a blocking
 *   waitpid().
 */

static void
shutdown_maybe_done(struct terminal *term)
{
    bool shutdown_done =
        term->window == NULL && term->shutdown.client_has_terminated;

    LOG_DBG("window=%p, slave-has-been-reaped=%d --> %s",
            (void *)term->window, term->shutdown.client_has_terminated,
            (shutdown_done
             ? "shutdown done, calling term_destroy()"
             : "no action"));

    if (!shutdown_done)
        return;

    void (*cb)(void *, int) = term->shutdown.cb;
    void *cb_data = term->shutdown.cb_data;

    int exit_code = term_destroy(term);
    if (cb != NULL)
        cb(cb_data, exit_code);
}

static void
fdm_client_terminated(struct reaper *reaper, pid_t pid, int status, void *data)
{
    struct terminal *term = data;
    LOG_DBG("slave (PID=%u) died", pid);

    term->shutdown.client_has_terminated = true;
    term->shutdown.exit_status = status;

    if (term->shutdown.terminate_timeout_fd >= 0) {
        fdm_del(term->fdm, term->shutdown.terminate_timeout_fd);
        term->shutdown.terminate_timeout_fd = -1;
    }

    if (term->shutdown.in_progress)
        shutdown_maybe_done(term);
    else if (!term->conf->hold_at_exit)
        term_shutdown(term);
}

static bool
fdm_shutdown(struct fdm *fdm, int fd, int events, void *data)
{
    struct terminal *term = data;

    /* Kill the event FD */
    fdm_del(term->fdm, fd);

    wayl_win_destroy(term->window);
    term->window = NULL;

    struct wayland *wayl = term->wl;

    /*
     * Normally we'd get unmapped when we destroy the Wayland
     * above.
     *
     * However, it appears that under certain conditions, those events
     * are deferred (for example, when a screen locker is active), and
     * thus we can get here without having been unmapped.
     */
    tll_foreach(wayl->seats, it) {
        if (it->item.kbd_focus == term)
            it->item.kbd_focus = NULL;
        if (it->item.mouse_focus == term)
            it->item.mouse_focus = NULL;
    }

    shutdown_maybe_done(term);
    return true;
}

static bool
fdm_terminate_timeout(struct fdm *fdm, int fd, int events, void *data)
{
    uint64_t unused;
    ssize_t bytes = read(fd, &unused, sizeof(unused));
    if (bytes < 0) {
        LOG_ERRNO("failed to read from slave terminate timeout FD");
        return false;
    }

    struct terminal *term = data;
    xassert(!term->shutdown.client_has_terminated);

    LOG_DBG("slave (PID=%u) has not terminated, sending SIGKILL (%d)",
            term->slave, SIGKILL);

    kill(-term->slave, SIGKILL);
    return true;
}

bool
term_shutdown(struct terminal *term)
{
    if (term->shutdown.in_progress)
        return true;

    term->shutdown.in_progress = true;

    /*
     * Close FDs then postpone self-destruction to the next poll
     * iteration, by creating an event FD that we trigger immediately.
     */

    term_cursor_blink_update(term);
    xassert(term->cursor_blink.fd < 0);

    fdm_del(term->fdm, term->selection.auto_scroll.fd);
    fdm_del(term->fdm, term->render.app_sync_updates.timer_fd);
    fdm_del(term->fdm, term->render.title.timer_fd);
    fdm_del(term->fdm, term->delayed_render_timer.lower_fd);
    fdm_del(term->fdm, term->delayed_render_timer.upper_fd);
    fdm_del(term->fdm, term->blink.fd);
    fdm_del(term->fdm, term->flash.fd);

    if (term->window != NULL && term->window->is_configured)
        fdm_del(term->fdm, term->ptmx);
    else
        close(term->ptmx);

    if (!term->shutdown.client_has_terminated) {
        LOG_DBG("initiating asynchronous terminate of slave; "
                "sending SIGTERM to PID=%u", term->slave);

        kill(-term->slave, SIGTERM);

        const struct itimerspec timeout = {.it_value = {.tv_sec = 60}};

        int timeout_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (timeout_fd < 0 ||
            timerfd_settime(timeout_fd, 0, &timeout, NULL) < 0 ||
            !fdm_add(term->fdm, timeout_fd, EPOLLIN, &fdm_terminate_timeout, term))
        {
            if (timeout_fd >= 0)
                close(timeout_fd);
            LOG_ERRNO("failed to create slave terminate timeout FD");
            return false;
        }

        xassert(term->shutdown.terminate_timeout_fd < 0);
        term->shutdown.terminate_timeout_fd = timeout_fd;
    }

    term->selection.auto_scroll.fd = -1;
    term->render.app_sync_updates.timer_fd = -1;
    term->render.title.timer_fd = -1;
    term->delayed_render_timer.lower_fd = -1;
    term->delayed_render_timer.upper_fd = -1;
    term->blink.fd = -1;
    term->flash.fd = -1;
    term->ptmx = -1;

    int event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
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

    key_binding_unref_term(term->wl->key_binding_manager, term);

    tll_foreach(term->wl->terms, it) {
        if (it->item == term) {
            tll_remove(term->wl->terms, it);
            break;
        }
    }

    fdm_del(term->fdm, term->selection.auto_scroll.fd);
    fdm_del(term->fdm, term->render.app_sync_updates.timer_fd);
    fdm_del(term->fdm, term->render.title.timer_fd);
    fdm_del(term->fdm, term->delayed_render_timer.lower_fd);
    fdm_del(term->fdm, term->delayed_render_timer.upper_fd);
    fdm_del(term->fdm, term->cursor_blink.fd);
    fdm_del(term->fdm, term->blink.fd);
    fdm_del(term->fdm, term->flash.fd);
    fdm_del(term->fdm, term->ptmx);
    if (term->shutdown.terminate_timeout_fd >= 0)
        fdm_del(term->fdm, term->shutdown.terminate_timeout_fd);

    if (term->window != NULL) {
        wayl_win_destroy(term->window);
        term->window = NULL;
    }

    mtx_lock(&term->render.workers.lock);
    xassert(tll_length(term->render.workers.queue) == 0);

    /* Count livinig threads - we may get here when only some of the
     * threads have been successfully started */
    size_t worker_count = 0;
    if (term->render.workers.threads != NULL) {
        for (size_t i = 0; i < term->render.workers.count; i++, worker_count++) {
            if (term->render.workers.threads[i] == 0)
                break;
        }

        for (size_t i = 0; i < worker_count; i++) {
            sem_post(&term->render.workers.start);
            tll_push_back(term->render.workers.queue, -2);
        }
    }
    mtx_unlock(&term->render.workers.lock);

    urls_reset(term);

    free(term->vt.osc.data);
    free(term->vt.osc8.uri);

    composed_free(term->composed);

    free(term->window_title);
    tll_free_and_free(term->window_title_stack, free);

    for (size_t i = 0; i < sizeof(term->fonts) / sizeof(term->fonts[0]); i++)
        fcft_destroy(term->fonts[i]);
    for (size_t i = 0; i < 4; i++)
        free(term->font_sizes[i]);


    free_custom_glyphs(
        &term->custom_glyphs.box_drawing, GLYPH_BOX_DRAWING_COUNT);
    free_custom_glyphs(
        &term->custom_glyphs.braille, GLYPH_BRAILLE_COUNT);
    free_custom_glyphs(
        &term->custom_glyphs.legacy, GLYPH_LEGACY_COUNT);

    free(term->search.buf);
    free(term->search.last.buf);

    if (term->render.workers.threads != NULL) {
        for (size_t i = 0; i < term->render.workers.count; i++) {
            if (term->render.workers.threads[i] != 0)
                thrd_join(term->render.workers.threads[i], NULL);
        }
    }
    free(term->render.workers.threads);
    mtx_destroy(&term->render.workers.lock);
    sem_destroy(&term->render.workers.start);
    sem_destroy(&term->render.workers.done);
    xassert(tll_length(term->render.workers.queue) == 0);
    tll_free(term->render.workers.queue);

    shm_unref(term->render.last_buf);
    shm_chain_free(term->render.chains.grid);
    shm_chain_free(term->render.chains.search);
    shm_chain_free(term->render.chains.scrollback_indicator);
    shm_chain_free(term->render.chains.render_timer);
    shm_chain_free(term->render.chains.url);
    shm_chain_free(term->render.chains.csd);
    shm_chain_free(term->render.chains.overlay);
    pixman_region32_fini(&term->render.last_overlay_clip);

    tll_free(term->tab_stops);

    tll_foreach(term->ptmx_buffers, it) {
        free(it->item.data);
        tll_remove(term->ptmx_buffers, it);
    }
    tll_foreach(term->ptmx_paste_buffers, it) {
        free(it->item.data);
        tll_remove(term->ptmx_paste_buffers, it);
    }

    sixel_fini(term);

    term_ime_reset(term);

    grid_free(&term->normal);
    grid_free(&term->alt);

    free(term->foot_exe);
    free(term->cwd);
    free(term->mouse_user_cursor);

    int ret = EXIT_SUCCESS;

    if (term->slave > 0) {
        /* We’ll deal with this explicitly */
        reaper_del(term->reaper, term->slave);

        int exit_status;

        if (term->shutdown.client_has_terminated)
            exit_status = term->shutdown.exit_status;
        else {
            LOG_DBG("initiating blocking terminate of slave; "
                    "sending SIGTERM to PID=%u", term->slave);

            kill(-term->slave, SIGTERM);

            /*
             * we’ve closed the ptxm, and sent SIGTERM to the client
             * application. It *should* exit...
             *
             * But, since it is possible to write clients that ignore
             * this, we need to handle it in *some* way.
             *
             * So, what we do is register a SIGALRM handler, and configure a 30
             * second alarm. If the slave hasn't died after this time, we send
             * it a SIGKILL,
             *
             * Note that this solution is *not* asynchronous, and any
             * other events etc will be ignored during this time. This of
             * course only applies to a 'foot --server' instance, where
             * there might be other terminals running.
             */
            struct sigaction action = {.sa_handler = &sig_alarm};
            sigemptyset(&action.sa_mask);
            sigaction(SIGALRM, &action, NULL);
            alarm(60);

            while (true) {
                int r = waitpid(term->slave, &exit_status, 0);

                if (r == term->slave)
                    break;

                if (r == -1) {
                    xassert(errno == EINTR);

                    if (alarm_raised) {
                        LOG_DBG(
                            "slave (PID=%u) has not terminate yet, "
                            "sending: SIGKILL (%d)", term->slave, SIGKILL);

                        kill(-term->slave, SIGKILL);
                    }
                }
            }

            /* Cancel alarm */
            alarm(0);
            action.sa_handler = SIG_DFL;
            sigaction(SIGALRM, &action, NULL);
        }

        ret = EXIT_FAILURE;
        if (WIFEXITED(exit_status)) {
            ret = WEXITSTATUS(exit_status);
            LOG_DBG("slave exited with code %d", ret);
        } else if (WIFSIGNALED(exit_status)) {
            ret = WTERMSIG(exit_status);
            LOG_WARN("slave exited with signal %d (%s)", ret, strsignal(ret));
        } else {
            LOG_WARN("slave exited for unknown reason (status = 0x%08x)",
                     exit_status);
        }
    }

    free(term);

#if defined(__GLIBC__)
    if (!malloc_trim(0))
        LOG_WARN("failed to trim memory");
#endif

    return ret;
}

static inline void
erase_cell_range(struct terminal *term, struct row *row, int start, int end)
{
    xassert(start < term->cols);
    xassert(end < term->cols);

    row->dirty = true;

    const enum color_source bg_src = term->vt.attrs.bg_src;

    if (unlikely(bg_src != COLOR_DEFAULT)) {
        for (int col = start; col <= end; col++) {
            struct cell *c = &row->cells[col];
            c->wc = 0;
            c->attrs = (struct attributes){.bg_src = bg_src, .bg = term->vt.attrs.bg};
        }
    } else
        memset(&row->cells[start], 0, (end - start + 1) * sizeof(row->cells[0]));

    if (unlikely(row->extra != NULL))
        grid_row_uri_range_erase(row, start, end);
}

static inline void
erase_line(struct terminal *term, struct row *row)
{
    erase_cell_range(term, row, 0, term->cols - 1);
    row->linebreak = false;
    row->prompt_marker = false;
}

void
term_reset(struct terminal *term, bool hard)
{
    term->cursor_keys_mode = CURSOR_KEYS_NORMAL;
    term->keypad_keys_mode = KEYPAD_NUMERICAL;
    term->reverse = false;
    term->hide_cursor = false;
    term->reverse_wrap = true;
    term->auto_margin = true;
    term->insert_mode = false;
    term->bracketed_paste = false;
    term->focus_events = false;
    term->num_lock_modifier = true;
    term->bell_action_enabled = true;
    term->mouse_tracking = MOUSE_NONE;
    term->mouse_reporting = MOUSE_NORMAL;
    term->charsets.selected = G0;
    term->charsets.set[G0] = CHARSET_ASCII;
    term->charsets.set[G1] = CHARSET_ASCII;
    term->charsets.set[G2] = CHARSET_ASCII;
    term->charsets.set[G3] = CHARSET_ASCII;
    term->saved_charsets = term->charsets;
    tll_free_and_free(term->window_title_stack, free);
    term_set_window_title(term, term->conf->title);

    term_set_user_mouse_cursor(term, NULL);

    memset(term->normal.kitty_kbd.flags, 0, sizeof(term->normal.kitty_kbd.flags));
    memset(term->alt.kitty_kbd.flags, 0, sizeof(term->alt.kitty_kbd.flags));
    term->normal.kitty_kbd.idx = term->alt.kitty_kbd.idx = 0;

    term->scroll_region.start = 0;
    term->scroll_region.end = term->rows;

    free(term->vt.osc8.uri);
    free(term->vt.osc.data);

    term->vt = (struct vt){
        .state = 0,     /* STATE_GROUND */
    };

    if (term->grid == &term->alt) {
        term->grid = &term->normal;
        selection_cancel(term);
    }

    term->meta.esc_prefix = true;
    term->meta.eight_bit = true;

    tll_foreach(term->normal.sixel_images, it) {
        sixel_destroy(&it->item);
        tll_remove(term->normal.sixel_images, it);
    }
    tll_foreach(term->alt.sixel_images, it) {
        sixel_destroy(&it->item);
        tll_remove(term->alt.sixel_images, it);
    }

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    term_ime_enable(term);
#endif

    term_update_ascii_printer(term);

    if (!hard)
        return;

    term->flash.active = false;
    term->blink.state = BLINK_ON;
    fdm_del(term->fdm, term->blink.fd); term->blink.fd = -1;
    term->colors.fg = term->conf->colors.fg;
    term->colors.bg = term->conf->colors.bg;
    term->colors.alpha = term->conf->colors.alpha;
    term->colors.selection_fg = term->conf->colors.selection_fg;
    term->colors.selection_bg = term->conf->colors.selection_bg;
    term->colors.use_custom_selection = term->conf->colors.use_custom.selection;
    memcpy(term->colors.table, term->conf->colors.table,
           sizeof(term->colors.table));
    term->origin = ORIGIN_ABSOLUTE;
    term->normal.cursor.lcf = false;
    term->alt.cursor.lcf = false;
    term->normal.cursor = (struct cursor){.point = {0, 0}};
    term->normal.saved_cursor = (struct cursor){.point = {0, 0}};
    term->alt.cursor = (struct cursor){.point = {0, 0}};
    term->alt.saved_cursor = (struct cursor){.point = {0, 0}};
    term->cursor_style = term->conf->cursor.style;
    term->cursor_blink.decset = false;
    term->cursor_blink.deccsusr = term->conf->cursor.blink;
    term_cursor_blink_update(term);
    term->cursor_color.text = term->conf->cursor.color.text;
    term->cursor_color.cursor = term->conf->cursor.color.cursor;
    selection_cancel(term);
    term->normal.offset = term->normal.view = 0;
    term->alt.offset = term->alt.view = 0;
    for (size_t i = 0; i < term->rows; i++) {
        struct row *r = grid_row_and_alloc(&term->normal, i);
        erase_line(term, r);
    }
    for (size_t i = 0; i < term->rows; i++) {
        struct row *r = grid_row_and_alloc(&term->alt, i);
        erase_line(term, r);
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
    tll_free(term->normal.scroll_damage);
    tll_free(term->alt.scroll_damage);
    term->render.last_cursor.row = NULL;
    term_damage_all(term);

    term->sixel.scrolling = true;
    term->sixel.cursor_right_of_graphics = false;
    term->sixel.use_private_palette = true;
    term->sixel.max_width = SIXEL_MAX_WIDTH;
    term->sixel.max_height = SIXEL_MAX_HEIGHT;
    term->sixel.palette_size = SIXEL_MAX_COLORS;
    free(term->sixel.private_palette);
    free(term->sixel.shared_palette);
    term->sixel.private_palette = term->sixel.shared_palette = NULL;
}

static bool
term_font_size_adjust(struct terminal *term, double amount)
{
    const struct config *conf = term->conf;

    const float dpi = term->font_is_sized_by_dpi ? term->font_dpi : 96.;

    for (size_t i = 0; i < 4; i++) {
        const struct config_font_list *font_list = &conf->fonts[i];

        for (size_t j = 0; j < font_list->count; j++) {
            float old_pt_size = term->font_sizes[i][j].pt_size;

            /*
             * To ensure primary and user-configured fallback fonts are
             * resizes by the same amount, convert pixel sizes to point
             * sizes, and to the adjustment on point sizes only.
             */

            if (term->font_sizes[i][j].px_size > 0)
                old_pt_size = term->font_sizes[i][j].px_size * 72. / dpi;

            term->font_sizes[i][j].pt_size = fmaxf(old_pt_size + amount, 0.);
            term->font_sizes[i][j].px_size = -1;
        }
    }

    if (term->font_line_height.px >= 0) {
        float old_pt_size = term->font_line_height.px > 0
            ? term->font_line_height.px * 72. / dpi
            : term->font_line_height.pt;

        term->font_line_height.px = 0;
        term->font_line_height.pt = fmaxf(old_pt_size + amount, 0.);
    }

    return reload_fonts(term);
}

bool
term_font_size_increase(struct terminal *term)
{
    if (!term_font_size_adjust(term, 0.5))
        return false;

    return true;
}

bool
term_font_size_decrease(struct terminal *term)
{
    if (!term_font_size_adjust(term, -0.5))
        return false;

    return true;
}

bool
term_font_size_reset(struct terminal *term)
{
    return load_fonts_from_conf(term);
}

bool
term_font_dpi_changed(struct terminal *term, int old_scale)
{
    float dpi = get_font_dpi(term);
    xassert(term->scale > 0);

    bool was_scaled_using_dpi = term->font_is_sized_by_dpi;
    bool will_scale_using_dpi = term_font_size_by_dpi(term);

    bool need_font_reload =
        was_scaled_using_dpi != will_scale_using_dpi ||
        (will_scale_using_dpi
         ? term->font_dpi != dpi
         : old_scale != term->scale);

    if (need_font_reload) {
        LOG_DBG("DPI/scale change: DPI-awareness=%s, "
                "DPI: %.2f -> %.2f, scale: %d -> %d, "
                "sizing font based on monitor's %s",
                term->conf->dpi_aware == DPI_AWARE_AUTO ? "auto" :
                term->conf->dpi_aware == DPI_AWARE_YES ? "yes" : "no",
                term->font_dpi, dpi, old_scale, term->scale,
                will_scale_using_dpi ? "DPI" : "scaling factor");
    }

    term->font_dpi = dpi;
    term->font_is_sized_by_dpi = will_scale_using_dpi;

    if (!need_font_reload)
        return true;

    return reload_fonts(term);
}

void
term_font_subpixel_changed(struct terminal *term)
{
    enum fcft_subpixel subpixel = get_font_subpixel(term);

    if (term->font_subpixel == subpixel)
        return;

#if defined(_DEBUG) && LOG_ENABLE_DBG
    static const char *const str[] = {
        [FCFT_SUBPIXEL_DEFAULT] = "default",
        [FCFT_SUBPIXEL_NONE] = "disabled",
        [FCFT_SUBPIXEL_HORIZONTAL_RGB] = "RGB",
        [FCFT_SUBPIXEL_HORIZONTAL_BGR] = "BGR",
        [FCFT_SUBPIXEL_VERTICAL_RGB] = "V-RGB",
        [FCFT_SUBPIXEL_VERTICAL_BGR] = "V-BGR",
    };

    LOG_DBG("subpixel mode changed: %s -> %s", str[term->font_subpixel], str[subpixel]);
#endif

    term->font_subpixel = subpixel;
    term_damage_view(term);
    render_refresh(term);
}

void
term_damage_rows(struct terminal *term, int start, int end)
{
    xassert(start <= end);
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
    xassert(start <= end);
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
term_damage_cursor(struct terminal *term)
{
    term->grid->cur_row->cells[term->grid->cursor.point.col].attrs.clean = 0;
    term->grid->cur_row->dirty = true;
}

void
term_damage_margins(struct terminal *term)
{
    term->render.margins = true;
}

void
term_damage_scroll(struct terminal *term, enum damage_type damage_type,
                   struct scroll_region region, int lines)
{
    if (tll_length(term->grid->scroll_damage) > 0) {
        struct damage *dmg = &tll_back(term->grid->scroll_damage);

        if (dmg->type == damage_type &&
            dmg->region.start == region.start &&
            dmg->region.end == region.end)
        {
            dmg->lines += lines;
            return;
        }
    }
    struct damage dmg = {
        .type = damage_type,
        .region = region,
        .lines = lines,
    };
    tll_push_back(term->grid->scroll_damage, dmg);
}

void
term_erase(struct terminal *term, int start_row, int start_col,
           int end_row, int end_col)
{
    xassert(start_row <= end_row);
    xassert(start_col <= end_col || start_row < end_row);

    if (start_row == end_row) {
        struct row *row = grid_row(term->grid, start_row);
        erase_cell_range(term, row, start_col, end_col);
        sixel_overwrite_by_row(term, start_row, start_col, end_col - start_col + 1);
        return;
    }

    xassert(end_row > start_row);

    erase_cell_range(
        term, grid_row(term->grid, start_row), start_col, term->cols - 1);
    sixel_overwrite_by_row(term, start_row, start_col, term->cols - start_col);

    for (int r = start_row + 1; r < end_row; r++)
        erase_line(term, grid_row(term->grid, r));
    sixel_overwrite_by_rectangle(
        term, start_row + 1, 0, end_row - start_row, term->cols);

    erase_cell_range(term, grid_row(term->grid, end_row), 0, end_col);
    sixel_overwrite_by_row(term, end_row, 0, end_col + 1);
}

void
term_erase_scrollback(struct terminal *term)
{
    const struct grid *grid = term->grid;
    const int num_rows = grid->num_rows;
    const int mask = num_rows - 1;

    const int start = (grid->offset + term->rows) & mask;
    const int end = (grid->offset - 1) & mask;

    const int rel_start = grid_row_abs_to_sb(grid, term->rows, start);
    const int rel_end = grid_row_abs_to_sb(grid, term->rows, end);

    const int sel_start = selection_get_start(term).row;
    const int sel_end = selection_get_end(term).row;

    if (sel_end >= 0) {
        /*
         * Cancel selection if it touches any of the rows in the
         * scrollback, since we can’t have the selection reference
         * soon-to-be deleted rows.
         *
         * This is done by range checking the selection range against
         * the scrollback range.
         *
         * To make this comparison simpler, the start/end absolute row
         * numbers are “rebased” against the scrollback start, where
         * row 0 is the *first* row in the scrollback. A high number
         * thus means the row is further *down* in the scrollback,
         * closer to the screen bottom.
         */

        const int rel_sel_start = grid_row_abs_to_sb(grid, term->rows, sel_start);
        const int rel_sel_end = grid_row_abs_to_sb(grid, term->rows, sel_end);

        if ((rel_sel_start <= rel_start && rel_sel_end >= rel_start) ||
            (rel_sel_start <= rel_end && rel_sel_end >= rel_end) ||
            (rel_sel_start >= rel_start && rel_sel_end <= rel_end))
        {
            selection_cancel(term);
        }
    }

    tll_foreach(term->grid->sixel_images, it) {
        struct sixel *six = &it->item;
        const int six_start = grid_row_abs_to_sb(grid, term->rows, six->pos.row);
        const int six_end = grid_row_abs_to_sb(
            grid, term->rows, six->pos.row + six->rows - 1);

        if ((six_start <= rel_start && six_end >= rel_start) ||
            (six_start <= rel_end && six_end >= rel_end) ||
            (six_start >= rel_start && six_end <= rel_end))
        {
            sixel_destroy(six);
            tll_remove(term->grid->sixel_images, it);
        }
    }

    for (int i = start;; i = (i + 1) & mask) {
        struct row *row = term->grid->rows[i];
        if (row != NULL) {
            if (term->render.last_cursor.row == row)
                term->render.last_cursor.row = NULL;

            grid_row_free(row);
            term->grid->rows[i] = NULL;
        }

        if (i == end)
            break;
    }

    term->grid->view = term->grid->offset;
    term_damage_view(term);
}

UNITTEST
{
    const int scrollback_rows = 16;
    const int term_rows = 5;
    const int cols = 5;

    struct fdm *fdm = fdm_init();
    xassert(fdm != NULL);

    struct terminal term = {
        .fdm = fdm,
        .rows = term_rows,
        .cols = cols,
        .normal = {
            .rows = xcalloc(scrollback_rows, sizeof(term.normal.rows[0])),
            .num_rows = scrollback_rows,
            .num_cols = cols,
        },
        .grid = &term.normal,
        .selection = {
            .coords = {
                .start = {-1, -1},
                .end = {-1, -1},
            },
            .kind = SELECTION_NONE,
            .auto_scroll = {
                .fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK),
            },
        },
    };

    xassert(term.selection.auto_scroll.fd >= 0);

#define populate_scrollback() do {                                      \
        for (int i = 0; i < scrollback_rows; i++) {                     \
            if (term.normal.rows[i] == NULL) {                          \
                struct row *r = xcalloc(1, sizeof(*term.normal.rows[i])); \
                r->cells = xcalloc(cols, sizeof(r->cells[0]));          \
                term.normal.rows[i] = r;                                \
            }                                                           \
        }                                                               \
    } while (0)

    /*
     * Test case 1 - no selection, just verify all rows except those
     * on screen have been deleted.
     */

    populate_scrollback();
    term.normal.offset = 11;
    term_erase_scrollback(&term);
    for (int i = 0; i < scrollback_rows; i++) {
        if (i >= term.normal.offset && i < term.normal.offset + term_rows)
            xassert(term.normal.rows[i] != NULL);
        else
            xassert(term.normal.rows[i] == NULL);
    }

    /*
     * Test case 2 - selection that touches the scrollback. Verify the
     * selection is cancelled.
     */

    term.normal.offset = 14;  /* Screen covers rows 14,15,0,1,2 */

    /* Selection covers rows 15,0,1,2,3 */
    term.selection.coords.start = (struct coord){.row = 15};
    term.selection.coords.end = (struct coord){.row = 19};
    term.selection.kind = SELECTION_CHAR_WISE;

    populate_scrollback();
    term_erase_scrollback(&term);
    xassert(term.selection.coords.start.row < 0);
    xassert(term.selection.coords.end.row < 0);
    xassert(term.selection.kind == SELECTION_NONE);

    /*
     * Test case 3 - selection that does *not* touch the
     * scrollback. Verify the selection is *not* cancelled.
     */

    /* Selection covers rows 15,0 */
    term.selection.coords.start = (struct coord){.row = 15};
    term.selection.coords.end = (struct coord){.row = 16};
    term.selection.kind = SELECTION_CHAR_WISE;

    populate_scrollback();
    term_erase_scrollback(&term);
    xassert(term.selection.coords.start.row == 15);
    xassert(term.selection.coords.end.row == 16);
    xassert(term.selection.kind == SELECTION_CHAR_WISE);

    term.selection.coords.start = (struct coord){-1, -1};
    term.selection.coords.end = (struct coord){-1, -1};
    term.selection.kind = SELECTION_NONE;

    /*
     * Test case 4 - sixel that touch the scrollback
     */

    struct sixel six = {
        .rows = 5,
        .pos = {
            .row = 15,
        },
    };
    tll_push_back(term.normal.sixel_images, six);
    populate_scrollback();
    term_erase_scrollback(&term);
    xassert(tll_length(term.normal.sixel_images) == 0);

    /*
     * Test case 5 - sixel that does *not* touch the scrollback
     */
    six.rows = 3;
    tll_push_back(term.normal.sixel_images, six);
    populate_scrollback();
    term_erase_scrollback(&term);
    xassert(tll_length(term.normal.sixel_images) == 1);

    /* Cleanup */
    tll_free(term.normal.sixel_images);
    close(term.selection.auto_scroll.fd);
    for (int i = 0; i < scrollback_rows; i++)
        grid_row_free(term.normal.rows[i]);
    free(term.normal.rows);
    fdm_destroy(fdm);
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

    BUG("Invalid cursor_origin value");
    return -1;
}

void
term_cursor_to(struct terminal *term, int row, int col)
{
    xassert(row < term->rows);
    xassert(col < term->cols);

    term->grid->cursor.lcf = false;

    term->grid->cursor.point.col = col;
    term->grid->cursor.point.row = row;

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
    int move_amount = min(term->grid->cursor.point.col, count);
    term->grid->cursor.point.col -= move_amount;
    xassert(term->grid->cursor.point.col >= 0);
    term->grid->cursor.lcf = false;
}

void
term_cursor_right(struct terminal *term, int count)
{
    int move_amount = min(term->cols - term->grid->cursor.point.col - 1, count);
    term->grid->cursor.point.col += move_amount;
    xassert(term->grid->cursor.point.col < term->cols);
    term->grid->cursor.lcf = false;
}

void
term_cursor_up(struct terminal *term, int count)
{
    int top = term->origin == ORIGIN_ABSOLUTE ? 0 : term->scroll_region.start;
    xassert(term->grid->cursor.point.row >= top);

    int move_amount = min(term->grid->cursor.point.row - top, count);
    term_cursor_to(term, term->grid->cursor.point.row - move_amount, term->grid->cursor.point.col);
}

void
term_cursor_down(struct terminal *term, int count)
{
    int bottom = term->origin == ORIGIN_ABSOLUTE ? term->rows : term->scroll_region.end;
    xassert(bottom >= term->grid->cursor.point.row);

    int move_amount = min(bottom - term->grid->cursor.point.row - 1, count);
    term_cursor_to(term, term->grid->cursor.point.row + move_amount, term->grid->cursor.point.col);
}

static bool
cursor_blink_rearm_timer(struct terminal *term)
{
    if (term->cursor_blink.fd < 0) {
        int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (fd < 0) {
            LOG_ERRNO("failed to create cursor blink timer FD");
            return false;
        }

        if (!fdm_add(term->fdm, fd, EPOLLIN, &fdm_cursor_blink, term)) {
            close(fd);
            return false;
        }

        term->cursor_blink.fd = fd;
    }

    static const struct itimerspec timer = {
        .it_value = {.tv_sec = 0, .tv_nsec = 500000000},
        .it_interval = {.tv_sec = 0, .tv_nsec = 500000000},
    };

    if (timerfd_settime(term->cursor_blink.fd, 0, &timer, NULL) < 0) {
        LOG_ERRNO("failed to arm cursor blink timer");
        fdm_del(term->fdm, term->cursor_blink.fd);
        term->cursor_blink.fd = -1;
        return false;
    }

    return true;
}

static bool
cursor_blink_disarm_timer(struct terminal *term)
{
    fdm_del(term->fdm, term->cursor_blink.fd);
    term->cursor_blink.fd = -1;
    return true;
}

void
term_cursor_blink_update(struct terminal *term)
{
    bool enable = term->cursor_blink.decset || term->cursor_blink.deccsusr;
    bool activate = !term->shutdown.in_progress && enable && term->visual_focus;

    LOG_DBG("decset=%d, deccsrusr=%d, focus=%d, shutting-down=%d, enable=%d, activate=%d",
            term->cursor_blink.decset, term->cursor_blink.deccsusr,
            term->visual_focus, term->shutdown.in_progress,
            enable, activate);

    if (activate && term->cursor_blink.fd < 0) {
        term->cursor_blink.state = CURSOR_BLINK_ON;
        cursor_blink_rearm_timer(term);
    } else if (!activate && term->cursor_blink.fd >= 0)
        cursor_blink_disarm_timer(term);
}

static bool
selection_on_top_region(const struct terminal *term,
                        struct scroll_region region)
{
    return region.start > 0 &&
        selection_on_rows(term, 0, region.start - 1);
}

static bool
selection_on_bottom_region(const struct terminal *term,
                           struct scroll_region region)
{
    return region.end < term->rows &&
        selection_on_rows(term, region.end, term->rows - 1);
}

void
term_scroll_partial(struct terminal *term, struct scroll_region region, int rows)
{
    LOG_DBG("scroll: rows=%d, region.start=%d, region.end=%d",
            rows, region.start, region.end);

    /* Verify scroll amount has been clamped */
    xassert(rows <= region.end - region.start);

    /* Cancel selections that cannot be scrolled */
    if (unlikely(term->selection.coords.end.row >= 0)) {
        /*
         * Selection is (partly) inside either the top or bottom
         * scrolling regions, or on (at least one) of the lines
         * scrolled in (i.e. re-used lines).
         */
        if (selection_on_top_region(term, region) ||
            selection_on_bottom_region(term, region) ||
            selection_on_rows(term, region.end - rows, region.end - 1))
        {
            selection_cancel(term);
        }
    }

    sixel_scroll_up(term, rows);

    /* How many lines from the scrollback start is the current viewport? */
    int view_sb_start_distance = grid_row_abs_to_sb(
        term->grid, term->rows, term->grid->view);

    bool view_follows = term->grid->view == term->grid->offset;
    term->grid->offset += rows;
    term->grid->offset &= term->grid->num_rows - 1;

    if (likely(view_follows)) {
        selection_view_down(term, term->grid->offset);
        term->grid->view = term->grid->offset;
    } else if (unlikely(rows > view_sb_start_distance)) {
        /* Part of current view is being scrolled out */
        int new_view = grid_row_sb_to_abs(term->grid, term->rows, 0);
        selection_view_down(term, new_view);
        cmd_scrollback_down(term, rows - view_sb_start_distance);
    }

    /* Top non-scrolling region. */
    for (int i = region.start - 1; i >= 0; i--)
        grid_swap_row(term->grid, i - rows, i);

    /* Bottom non-scrolling region */
    for (int i = term->rows - 1; i >= region.end; i--)
        grid_swap_row(term->grid, i - rows, i);

    /* Erase scrolled in lines */
    for (int r = region.end - rows; r < region.end; r++) {
        struct row *row = grid_row_and_alloc(term->grid, r);
        erase_line(term, row);
    }

    term_damage_scroll(term, DAMAGE_SCROLL, region, rows);
    term->grid->cur_row = grid_row(term->grid, term->grid->cursor.point.row);

#if defined(_DEBUG)
    for (int r = 0; r < term->rows; r++)
        xassert(grid_row(term->grid, r) != NULL);
#endif
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

    /* Verify scroll amount has been clamped */
    xassert(rows <= region.end - region.start);

    /* Cancel selections that cannot be scrolled */
    if (unlikely(term->selection.coords.end.row >= 0)) {
        /*
         * Selection is (partly) inside either the top or bottom
         * scrolling regions, or on (at least one) of the lines
         * scrolled in (i.e. re-used lines).
         */
        if (selection_on_top_region(term, region) ||
            selection_on_bottom_region(term, region) ||
            selection_on_rows(term, region.start, region.start + rows - 1))
        {
            selection_cancel(term);
        }
    }

    sixel_scroll_down(term, rows);

    bool view_follows = term->grid->view == term->grid->offset;
    term->grid->offset -= rows;
    term->grid->offset += term->grid->num_rows;
    term->grid->offset &= term->grid->num_rows - 1;

    xassert(term->grid->offset >= 0);
    xassert(term->grid->offset < term->grid->num_rows);

    if (view_follows) {
        selection_view_up(term, term->grid->offset);
        term->grid->view = term->grid->offset;
    }

    /* Bottom non-scrolling region */
    for (int i = region.end + rows; i < term->rows + rows; i++)
        grid_swap_row(term->grid, i, i - rows);

    /* Top non-scrolling region */
    for (int i = 0 + rows; i < region.start + rows; i++)
        grid_swap_row(term->grid, i, i - rows);

    /* Erase scrolled in lines */
    for (int r = region.start; r < region.start + rows; r++) {
        struct row *row = grid_row_and_alloc(term->grid, r);
        erase_line(term, row);
    }

    term_damage_scroll(term, DAMAGE_SCROLL_REVERSE, region, rows);
    term->grid->cur_row = grid_row(term->grid, term->grid->cursor.point.row);

#if defined(_DEBUG)
    for (int r = 0; r < term->rows; r++)
        xassert(grid_row(term->grid, r) != NULL);
#endif
}

void
term_scroll_reverse(struct terminal *term, int rows)
{
    term_scroll_reverse_partial(term, term->scroll_region, rows);
}

void
term_carriage_return(struct terminal *term)
{
    term_cursor_left(term, term->grid->cursor.point.col);
}

void
term_linefeed(struct terminal *term)
{
    term->grid->cur_row->linebreak = true;
    term->grid->cursor.lcf = false;

    if (term->grid->cursor.point.row == term->scroll_region.end - 1)
        term_scroll(term, 1);
    else
        term_cursor_down(term, 1);
}

void
term_reverse_index(struct terminal *term)
{
    if (term->grid->cursor.point.row == term->scroll_region.start)
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
term_save_cursor(struct terminal *term)
{
    term->grid->saved_cursor = term->grid->cursor;
    term->vt.saved_attrs = term->vt.attrs;
    term->saved_charsets = term->charsets;
}

void
term_restore_cursor(struct terminal *term, const struct cursor *cursor)
{
    int row = min(cursor->point.row, term->rows - 1);
    int col = min(cursor->point.col, term->cols - 1);

    term_cursor_to(term, row, col);
    term->grid->cursor.lcf = cursor->lcf;

    term->vt.attrs = term->vt.saved_attrs;
    term->charsets = term->saved_charsets;
    term_update_ascii_printer(term);
}

void
term_visual_focus_in(struct terminal *term)
{
    if (term->visual_focus)
        return;

    term->visual_focus = true;
    term_cursor_blink_update(term);
    render_refresh_csd(term);
}

void
term_visual_focus_out(struct terminal *term)
{
    if (!term->visual_focus)
        return;

    term->visual_focus = false;
    term_cursor_blink_update(term);
    render_refresh_csd(term);
}

void
term_kbd_focus_in(struct terminal *term)
{
    if (term->kbd_focus)
        return;

    term->kbd_focus = true;

    if (term->render.urgency) {
        term->render.urgency = false;
        term_damage_margins(term);
    }

    cursor_refresh(term);

    if (term->focus_events)
        term_to_slave(term, "\033[I", 3);
}

void
term_kbd_focus_out(struct terminal *term)
{
    if (!term->kbd_focus)
        return;

    tll_foreach(term->wl->seats, it)
        if (it->item.kbd_focus == term)
            return;

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (term_ime_reset(term))
        render_refresh(term);
#endif

    term->kbd_focus = false;
    cursor_refresh(term);

    if (term->focus_events)
        term_to_slave(term, "\033[O", 3);
}

static int
linux_mouse_button_to_x(int button)
{
    switch (button) {
    case BTN_LEFT:        return 1;
    case BTN_MIDDLE:      return 2;
    case BTN_RIGHT:       return 3;
    case BTN_BACK:        return 4;
    case BTN_FORWARD:     return 5;
    case BTN_WHEEL_LEFT:  return 6;  /* Foot custom define */
    case BTN_WHEEL_RIGHT: return 7;  /* Foot custom define */
    case BTN_SIDE:        return 8;
    case BTN_EXTRA:       return 9;
    case BTN_TASK:        return -1;  /* TODO: ??? */

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

    case 4: case 5: case 6: case 7:
        /* Like button 1 and 2, but with 64 added */
        return xbutton - 4 + 64;

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
                   int row_pixels, int col_pixels, bool release)
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

    case MOUSE_SGR_PIXELS:
        snprintf(response, sizeof(response), "\033[<%d;%d;%d%c",
                 encoded_button, col_pixels + 1, row_pixels + 1, release ? 'm' : 'M');
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
report_mouse_motion(struct terminal *term, int encoded_button, int row, int col, int row_pixels, int col_pixels)
{
    report_mouse_click(term, encoded_button, row, col, row_pixels, col_pixels, false);
}

bool
term_mouse_grabbed(const struct terminal *term, const struct seat *seat)
{
    /*
     * Mouse is grabbed by us, regardless of whether mouse tracking
     * has been enabled or not.
     */

    xkb_mod_mask_t mods;
    get_current_modifiers(seat, &mods, NULL, 0);

    const struct key_binding_set *bindings =
        key_binding_for(term->wl->key_binding_manager, term, seat);
    const xkb_mod_mask_t override_modmask = bindings->selection_overrides;
    bool override_mods_pressed = (mods & override_modmask) == override_modmask;

    return term->mouse_tracking == MOUSE_NONE ||
        (seat->kbd_focus == term && override_mods_pressed);
}

void
term_mouse_down(struct terminal *term, int button, int row, int col,
                int row_pixels, int col_pixels,
                bool _shift, bool _alt, bool _ctrl)
{
    /* Map libevent button event code to X button number */
    int xbutton = linux_mouse_button_to_x(button);
    if (xbutton == -1)
        return;

    int encoded = encode_xbutton(xbutton);
    if (encoded == -1)
        return;


    bool has_focus = term->kbd_focus;
    bool shift = has_focus ? _shift : false;
    bool alt = has_focus ? _alt : false;
    bool ctrl = has_focus ? _ctrl : false;

    encoded += (shift ? 4 : 0) + (alt ? 8 : 0) + (ctrl ? 16 : 0);

    switch (term->mouse_tracking) {
    case MOUSE_NONE:
        break;

    case MOUSE_CLICK:
    case MOUSE_DRAG:
    case MOUSE_MOTION:
        report_mouse_click(term, encoded, row, col, row_pixels, col_pixels, false);
        break;

    case MOUSE_X10:
        /* Never enabled */
        BUG("X10 mouse mode not implemented");
        break;
    }
}

void
term_mouse_up(struct terminal *term, int button, int row, int col,
              int row_pixels, int col_pixels,
              bool _shift, bool _alt, bool _ctrl)
{
    /* Map libevent button event code to X button number */
    int xbutton = linux_mouse_button_to_x(button);
    if (xbutton == -1)
        return;

    if (xbutton == 4 || xbutton == 5) {
        /* No release events for vertical scroll wheel buttons */
        return;
    }

    int encoded = encode_xbutton(xbutton);
    if (encoded == -1)
        return;

    bool has_focus = term->kbd_focus;
    bool shift = has_focus ? _shift : false;
    bool alt = has_focus ? _alt : false;
    bool ctrl = has_focus ? _ctrl : false;

    encoded += (shift ? 4 : 0) + (alt ? 8 : 0) + (ctrl ? 16 : 0);

    switch (term->mouse_tracking) {
    case MOUSE_NONE:
        break;

    case MOUSE_CLICK:
    case MOUSE_DRAG:
    case MOUSE_MOTION:
        report_mouse_click(term, encoded, row, col, row_pixels, col_pixels, true);
        break;

    case MOUSE_X10:
        /* Never enabled */
        BUG("X10 mouse mode not implemented");
        break;
    }
}

void
term_mouse_motion(struct terminal *term, int button, int row, int col,
                  int row_pixels, int col_pixels,
                  bool _shift, bool _alt, bool _ctrl)
{
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

    bool has_focus = term->kbd_focus;
    bool shift = has_focus ? _shift : false;
    bool alt = has_focus ? _alt : false;
    bool ctrl = has_focus ? _ctrl : false;

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
        report_mouse_motion(term, encoded, row, col, row_pixels, col_pixels);
        break;

    case MOUSE_X10:
        /* Never enabled */
        BUG("X10 mouse mode not implemented");
        break;
    }
}

void
term_xcursor_update_for_seat(struct terminal *term, struct seat *seat)
{
    const char *xcursor = NULL;

    switch (term->active_surface) {
    case TERM_SURF_GRID: {
        bool have_custom_cursor =
            render_xcursor_is_valid(seat, term->mouse_user_cursor);

        xcursor = seat->pointer.hidden ? XCURSOR_HIDDEN
            : have_custom_cursor ? term->mouse_user_cursor
            : term->is_searching ? XCURSOR_LEFT_PTR
            : (seat->mouse.col >= 0 &&
               seat->mouse.row >= 0 &&
               term_mouse_grabbed(term, seat)) ? XCURSOR_TEXT
            : XCURSOR_LEFT_PTR;
        break;
    }
    case TERM_SURF_TITLE:
    case TERM_SURF_BUTTON_MINIMIZE:
    case TERM_SURF_BUTTON_MAXIMIZE:
    case TERM_SURF_BUTTON_CLOSE:
        xcursor = XCURSOR_LEFT_PTR;
        break;

    case TERM_SURF_BORDER_LEFT:
    case TERM_SURF_BORDER_RIGHT:
    case TERM_SURF_BORDER_TOP:
    case TERM_SURF_BORDER_BOTTOM:
        xcursor = xcursor_for_csd_border(term, seat->mouse.x, seat->mouse.y);
        break;

    case TERM_SURF_NONE:
        return;
    }

    if (xcursor == NULL)
        BUG("xcursor not set");

    render_xcursor_set(seat, term, xcursor);
}

void
term_xcursor_update(struct terminal *term)
{
    tll_foreach(term->wl->seats, it)
        term_xcursor_update_for_seat(term, &it->item);
}

void
term_set_window_title(struct terminal *term, const char *title)
{
    if (term->conf->locked_title && term->window_title_has_been_set)
        return;

    if (term->window_title != NULL && strcmp(term->window_title, title) == 0)
        return;

    free(term->window_title);
    term->window_title = xstrdup(title);
    render_refresh_title(term);
    term->window_title_has_been_set = true;
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

void
term_bell(struct terminal *term)
{
    if (!term->bell_action_enabled)
        return;

    if (term->conf->bell.urgent && !term->kbd_focus) {
        if (!wayl_win_set_urgent(term->window)) {
            /*
             * Urgency (xdg-activation) is relatively new in
             * Wayland. Fallback to our old, “faked”, urgency -
             * rendering our window margins in red
             */
            term->render.urgency = true;
            term_damage_margins(term);
        }
    }

    if (term->conf->bell.notify)
        notify_notify(term, "Bell", "Bell in terminal");

    if ((term->conf->bell.command.argv.args != NULL) &&
        (!term->kbd_focus || term->conf->bell.command_focused))
    {
        int devnull = open("/dev/null", O_RDONLY);
        spawn(term->reaper, NULL, term->conf->bell.command.argv.args,
              devnull, -1, -1, NULL);

        if (devnull >= 0)
            close(devnull);
    }
}

bool
term_spawn_new(const struct terminal *term)
{
    return spawn(
        term->reaper, term->cwd, (char *const []){term->foot_exe, NULL},
        -1, -1, -1, NULL);
}

void
term_enable_app_sync_updates(struct terminal *term)
{
    term->render.app_sync_updates.enabled = true;

    if (timerfd_settime(
            term->render.app_sync_updates.timer_fd, 0,
            &(struct itimerspec){.it_value = {.tv_sec = 1}}, NULL) < 0)
    {
        LOG_ERR("failed to arm timer for application synchronized updates");
    }

    /* Disable pending refresh *iff* the grid is the *only* thing
     * scheduled to be re-rendered */
    if (!term->render.refresh.csd && !term->render.refresh.search &&
        !term->render.pending.csd && !term->render.pending.search)
    {
        term->render.refresh.grid = false;
        term->render.pending.grid = false;
    }

    /* Disarm delayed rendering timers */
    timerfd_settime(
        term->delayed_render_timer.lower_fd, 0,
        &(struct itimerspec){{0}}, NULL);
    timerfd_settime(
        term->delayed_render_timer.upper_fd, 0,
        &(struct itimerspec){{0}}, NULL);
    term->delayed_render_timer.is_armed = false;
}

void
term_disable_app_sync_updates(struct terminal *term)
{
    if (!term->render.app_sync_updates.enabled)
        return;

    term->render.app_sync_updates.enabled = false;
    render_refresh(term);

    /* Reset timers */
    timerfd_settime(
        term->render.app_sync_updates.timer_fd, 0,
        &(struct itimerspec){{0}}, NULL);
}

static inline void
print_linewrap(struct terminal *term)
{
    if (likely(!term->grid->cursor.lcf)) {
        /* Not and end of line */
        return;
    }

    if (unlikely(!term->auto_margin)) {
        /* Auto-wrap disabled */
        return;
    }

    term->grid->cur_row->linebreak = false;
    term->grid->cursor.lcf = false;

    const int row = term->grid->cursor.point.row;

    if (row == term->scroll_region.end - 1)
        term_scroll(term, 1);
    else {
        const int new_row = min(row + 1, term->rows - 1);
        term->grid->cursor.point.row = new_row;
        term->grid->cur_row = grid_row(term->grid, new_row);
    }

    term->grid->cursor.point.col = 0;
}

static inline void
print_insert(struct terminal *term, int width)
{
    if (likely(!term->insert_mode))
        return;

    xassert(width > 0);

    struct row *row = term->grid->cur_row;
    const size_t move_count = max(0, term->cols - term->grid->cursor.point.col - width);

    memmove(
        &row->cells[term->grid->cursor.point.col + width],
        &row->cells[term->grid->cursor.point.col],
        move_count * sizeof(struct cell));

    /* Mark moved cells as dirty */
    for (size_t i = term->grid->cursor.point.col + width; i < term->cols; i++)
        row->cells[i].attrs.clean = 0;
}

static void
print_spacer(struct terminal *term, int col, int remaining)
{
    struct grid *grid = term->grid;
    struct row *row = grid->cur_row;
    struct cell *cell = &row->cells[col];

    cell->wc = CELL_SPACER + remaining;
    cell->attrs = term->vt.attrs;
}

void
term_print(struct terminal *term, char32_t wc, int width)
{
    xassert(width > 0);

    struct grid *grid = term->grid;

    if (unlikely(term->charsets.set[term->charsets.selected] == CHARSET_GRAPHIC) &&
        wc >= 0x60 && wc <= 0x7e)
    {
        /* 0x60 - 0x7e */
        static const char32_t vt100_0[] = {
            U'◆', U'▒', U'␉', U'␌', U'␍', U'␊', U'°', U'±', /* ` - g */
            U'␤', U'␋', U'┘', U'┐', U'┌', U'└', U'┼', U'⎺', /* h - o */
            U'⎻', U'─', U'⎼', U'⎽', U'├', U'┤', U'┴', U'┬', /* p - w */
            U'│', U'≤', U'≥', U'π', U'≠', U'£', U'·',       /* x - ~ */
        };

        xassert(width == 1);
        wc = vt100_0[wc - 0x60];
    }

    print_linewrap(term);
    print_insert(term, width);

    int col = grid->cursor.point.col;

    if (unlikely(width > 1) && likely(term->auto_margin) &&
        col + width > term->cols)
    {
        /* Multi-column character that doesn't fit on current line -
         * pad with spacers */
        for (size_t i = col; i < term->cols; i++)
            print_spacer(term, i, 0);

        /* And force a line-wrap */
        grid->cursor.lcf = 1;
        print_linewrap(term);
        col = 0;
    }

    sixel_overwrite_at_cursor(term, width);

    /* *Must* get current cell *after* linewrap+insert */
    struct row *row = grid->cur_row;
    row->dirty = true;
    row->linebreak = true;

    struct cell *cell = &row->cells[col];
    cell->wc = term->vt.last_printed = wc;
    cell->attrs = term->vt.attrs;

    if (term->vt.osc8.uri != NULL) {
        grid_row_uri_range_put(
            row, col, term->vt.osc8.uri, term->vt.osc8.id);

        switch (term->conf->url.osc8_underline) {
        case OSC8_UNDERLINE_ALWAYS:
            cell->attrs.url = true;
            break;

        case OSC8_UNDERLINE_URL_MODE:
            break;
        }
    } else if (row->extra != NULL)
        grid_row_uri_range_erase(row, col, col + width - 1);

    /* Advance cursor the 'additional' columns while dirty:ing the cells */
    for (int i = 1; i < width && col < term->cols - 1; i++) {
        col++;
        print_spacer(term, col, width - i);
    }

    /* Advance cursor */
    if (unlikely(++col >= term->cols)) {
        grid->cursor.lcf = true;
        col--;
    } else
        xassert(!grid->cursor.lcf);

    grid->cursor.point.col = col;
}

static void
ascii_printer_generic(struct terminal *term, char32_t wc)
{
    term_print(term, wc, 1);
}

static void
ascii_printer_fast(struct terminal *term, char32_t wc)
{
    struct grid *grid = term->grid;

    xassert(term->charsets.set[term->charsets.selected] == CHARSET_ASCII);
    xassert(!term->insert_mode);
    xassert(tll_length(grid->sixel_images) == 0);

    print_linewrap(term);

    /* *Must* get current cell *after* linewrap+insert */
    int col = grid->cursor.point.col;
    const int uri_start = col;

    struct row *row = grid->cur_row;
    row->dirty = true;
    row->linebreak = true;

    struct cell *cell = &row->cells[col];
    cell->wc = term->vt.last_printed = wc;
    cell->attrs = term->vt.attrs;

    /* Advance cursor */
    if (unlikely(++col >= term->cols)) {
        grid->cursor.lcf = true;
        col--;
    } else
        xassert(!grid->cursor.lcf);

    grid->cursor.point.col = col;

    if (unlikely(row->extra != NULL))
        grid_row_uri_range_erase(row, uri_start, uri_start);
}

static void
ascii_printer_single_shift(struct terminal *term, char32_t wc)
{
    ascii_printer_generic(term, wc);
    term->charsets.selected = term->charsets.saved;
    term_update_ascii_printer(term);
}

void
term_update_ascii_printer(struct terminal *term)
{
    void (*new_printer)(struct terminal *term, char32_t wc) =
        unlikely(tll_length(term->grid->sixel_images) > 0 ||
                 term->vt.osc8.uri != NULL ||
                 term->charsets.set[term->charsets.selected] == CHARSET_GRAPHIC ||
                 term->insert_mode)
        ? &ascii_printer_generic
        : &ascii_printer_fast;

#if defined(_DEBUG) && LOG_ENABLE_DBG
    if (term->ascii_printer != new_printer) {
        LOG_DBG("§switching ASCII printer %s -> %s",
                term->ascii_printer == &ascii_printer_fast ? "fast" : "generic",
                new_printer == &ascii_printer_fast ? "fast" : "generic");
    }
#endif

    term->ascii_printer = new_printer;
}

void
term_single_shift(struct terminal *term, enum charset_designator idx)
{
    term->charsets.saved = term->charsets.selected;
    term->charsets.selected = idx;
    term->ascii_printer = &ascii_printer_single_shift;
}

enum term_surface
term_surface_kind(const struct terminal *term, const struct wl_surface *surface)
{
    if (likely(surface == term->window->surface))
        return TERM_SURF_GRID;
    else if (surface == term->window->csd.surface[CSD_SURF_TITLE].surf)
        return TERM_SURF_TITLE;
    else if (surface == term->window->csd.surface[CSD_SURF_LEFT].surf)
        return TERM_SURF_BORDER_LEFT;
    else if (surface == term->window->csd.surface[CSD_SURF_RIGHT].surf)
        return TERM_SURF_BORDER_RIGHT;
    else if (surface == term->window->csd.surface[CSD_SURF_TOP].surf)
        return TERM_SURF_BORDER_TOP;
    else if (surface == term->window->csd.surface[CSD_SURF_BOTTOM].surf)
        return TERM_SURF_BORDER_BOTTOM;
    else if (surface == term->window->csd.surface[CSD_SURF_MINIMIZE].surf)
        return TERM_SURF_BUTTON_MINIMIZE;
    else if (surface == term->window->csd.surface[CSD_SURF_MAXIMIZE].surf)
        return TERM_SURF_BUTTON_MAXIMIZE;
    else if (surface == term->window->csd.surface[CSD_SURF_CLOSE].surf)
        return TERM_SURF_BUTTON_CLOSE;
    else
        return TERM_SURF_NONE;
}

static bool
rows_to_text(const struct terminal *term, int start, int end,
             char **text, size_t *len)
{
    struct extraction_context *ctx = extract_begin(SELECTION_NONE, true);
    if (ctx == NULL)
        return false;

    const int grid_rows = term->grid->num_rows;
    int r = start;

    while (true) {
        const struct row *row = term->grid->rows[r];
        xassert(row != NULL);

        for (int c = 0; c < term->cols; c++)
            if (!extract_one(term, row, &row->cells[c], c, ctx))
                goto out;

        if (r == end)
            break;

        r++;
        r &= grid_rows - 1;
    }

out:
    return extract_finish(ctx, text, len);
}

bool
term_scrollback_to_text(const struct terminal *term, char **text, size_t *len)
{
    const int grid_rows = term->grid->num_rows;
    int start = (term->grid->offset + term->rows) & (grid_rows - 1);
    int end = (term->grid->offset + term->rows - 1) & (grid_rows - 1);

    xassert(start >= 0);
    xassert(start < grid_rows);
    xassert(end >= 0);
    xassert(end < grid_rows);

    /* If scrollback isn't full yet, this may be NULL, so scan forward
     * until we find the first non-NULL row */
    while (term->grid->rows[start] == NULL) {
        start++;
        start &= grid_rows - 1;
    }

    while (term->grid->rows[end] == NULL) {
        end--;
        if (end < 0)
            end += term->grid->num_rows;
    }

    return rows_to_text(term, start, end, text, len);
}

bool
term_view_to_text(const struct terminal *term, char **text, size_t *len)
{
    int start = grid_row_absolute_in_view(term->grid, 0);
    int end = grid_row_absolute_in_view(term->grid, term->rows - 1);
    return rows_to_text(term, start, end, text, len);
}

bool
term_ime_is_enabled(const struct terminal *term)
{
#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    return term->ime_enabled;
#else
    return false;
#endif
}

void
term_ime_enable(struct terminal *term)
{
#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (term->ime_enabled)
        return;

    LOG_DBG("IME enabled");

    term->ime_enabled = true;

    /* IME is per seat - enable on all seat currently focusing us */
    tll_foreach(term->wl->seats, it) {
        if (it->item.kbd_focus == term)
            ime_enable(&it->item);
    }
#endif
}

void
term_ime_disable(struct terminal *term)
{
#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    if (!term->ime_enabled)
        return;

    LOG_DBG("IME disabled");

    term->ime_enabled = false;

    /* IME is per seat - disable on all seat currently focusing us */
    tll_foreach(term->wl->seats, it) {
        if (it->item.kbd_focus == term)
            ime_disable(&it->item);
    }
#endif
}

bool
term_ime_reset(struct terminal *term)
{
    bool at_least_one_seat_was_reset = false;

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    tll_foreach(term->wl->seats, it) {
        struct seat *seat = &it->item;

        if (seat->kbd_focus != term)
            continue;

        ime_reset_preedit(seat);
        at_least_one_seat_was_reset = true;
    }
#endif

    return at_least_one_seat_was_reset;
}

void
term_ime_set_cursor_rect(struct terminal *term, int x, int y, int width,
                         int height)
{
#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    tll_foreach(term->wl->seats, it) {
        if (it->item.kbd_focus == term) {
            it->item.ime.cursor_rect.pending.x = x;
            it->item.ime.cursor_rect.pending.y = y;
            it->item.ime.cursor_rect.pending.width = width;
            it->item.ime.cursor_rect.pending.height = height;
        }
    }
#endif
}

void
term_osc8_open(struct terminal *term, uint64_t id, const char *uri)
{
    term_osc8_close(term);
    xassert(term->vt.osc8.uri == NULL);

    term->vt.osc8.id = id;
    term->vt.osc8.uri = xstrdup(uri);
    term_update_ascii_printer(term);
}

void
term_osc8_close(struct terminal *term)
{
    free(term->vt.osc8.uri);
    term->vt.osc8.uri = NULL;
    term->vt.osc8.id = 0;
    term_update_ascii_printer(term);
}

void
term_set_user_mouse_cursor(struct terminal *term, const char *cursor)
{
    free(term->mouse_user_cursor);
    term->mouse_user_cursor = cursor != NULL ? xstrdup(cursor) : NULL;
    term_xcursor_update(term);
}
