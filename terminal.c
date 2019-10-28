#include "terminal.h"

#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>

#define LOG_MODULE "terminal"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "grid.h"
#include "render.h"
#include "vt.h"
#include "selection.h"
#include "config.h"
#include "tokenize.h"
#include "slave.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

struct terminal *
term_init(const struct config *conf, struct fdm *fdm, struct wayland *wayl,
          int argc, char *const *argv)
{
    struct terminal *term = malloc(sizeof(*term));
    *term = (struct terminal) {
        .fdm = fdm,
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
            .lower_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC),
            .upper_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC),
        },
    };

    LOG_INFO("using %zu rendering threads", term->render.workers.count);

    struct render_worker_context worker_context[term->render.workers.count];

    /* Initialize 'current' colors from the default colors */
    term->colors.fg = term->colors.default_fg;
    term->colors.bg = term->colors.default_bg;

    /* Initialize the 256 gray-scale color cube */
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
    if (term->ptmx == -1) {
        LOG_ERR("failed to open pseudo terminal");
        goto out;
    }

    if (term->flash.fd == -1 || term->blink.fd == -1) {
        LOG_ERR("failed to create timers");
        goto out;
    }

    sem_init(&term->render.workers.start, 0, 0);
    sem_init(&term->render.workers.done, 0, 0);
    mtx_init(&term->render.workers.lock, mtx_plain);
    cnd_init(&term->render.workers.cond);

    term->render.workers.threads = calloc(term->render.workers.count, sizeof(term->render.workers.threads[0]));
    for (size_t i = 0; i < term->render.workers.count; i++) {
        worker_context[i].term = term;
        worker_context[i].my_id = 1 + i;
        thrd_create(&term->render.workers.threads[i], &render_worker_thread, &worker_context[i]);
    }

    font_list_t font_names = tll_init();
    tll_foreach(conf->fonts, it)
        tll_push_back(font_names, it->item);

    if ((term->fonts[0] = font_from_name(font_names, "")) == NULL) {
        tll_free(font_names);
        goto out;
    }

    term->fonts[1] = font_from_name(font_names, "style=bold");
    term->fonts[2] = font_from_name(font_names, "style=italic");
    term->fonts[3] = font_from_name(font_names, "style=bold italic");

    tll_free(font_names);

    {
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
    }

    term->cell_width = (int)ceil(term->fextents.max_x_advance);
    term->cell_height = (int)ceil(term->fextents.height);
    LOG_INFO("cell width=%d, height=%d", term->cell_width, term->cell_height);

    /* Main window */
    term->window = wayl_win_init(wayl);
    if (term->window == NULL)
        goto out;

    term_set_window_title(term, "foot");

    unsigned width = conf->width;
    unsigned height = conf->height;

    if (width == -1) {
        assert(height == -1);
        width = 80 * term->cell_width;
        height = 24 * term->cell_height;
    }

    width = max(width, term->cell_width);
    height = max(height, term->cell_height);
    render_resize(term, width, height);

    {
        int fork_pipe[2];
        if (pipe2(fork_pipe, O_CLOEXEC) < 0) {
            LOG_ERRNO("failed to create pipe");
            goto out;
        }

        term->slave = fork();
        switch (term->slave) {
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
                if (!tokenize_cmdline(conf->shell, &_shell_argv)) {
                    (void)!write(fork_pipe[1], &errno, sizeof(errno));
                    _exit(0);
                }
                shell_argv = _shell_argv;
            }

            slave_spawn(term->ptmx, shell_argv, fork_pipe[1]);
            assert(false);
            break;

        default: {
            close(fork_pipe[1]); /* Close write end */
            LOG_DBG("slave has PID %d", term->slave);

            int _errno;
            static_assert(sizeof(errno) == sizeof(_errno), "errno size mismatch");

            ssize_t ret = read(fork_pipe[0], &_errno, sizeof(_errno));
            close(fork_pipe[0]);

            if (ret < 0) {
                LOG_ERRNO("failed to read from pipe");
                goto out;
            } else if (ret == sizeof(_errno)) {
                LOG_ERRNO(
                    "%s: failed to execute", argc == 0 ? conf->shell : argv[0]);
                goto out;
            } else
                LOG_DBG("%s: successfully started", conf->shell);
            break;
        }
        }
    }

    /* Read logic requires non-blocking mode */
    {
        int fd_flags = fcntl(term->ptmx, F_GETFL);
        if (fd_flags == -1) {
            LOG_ERRNO("failed to set non blocking mode on PTY master");
            goto out;
        }

        if (fcntl(term->ptmx, F_SETFL, fd_flags | O_NONBLOCK) == -1) {
            LOG_ERRNO("failed to set non blocking mode on PTY master");
            goto out;
        }
    }

    wayl->term = term;
    return term;

out:
    term_destroy(term);
    return NULL;
}

int
term_destroy(struct terminal *term)
{
    if (term == NULL)
        return 0;

    wayl_win_destroy(term->window);

    if (term->delayed_render_timer.lower_fd != -1)
        close(term->delayed_render_timer.lower_fd);
    if (term->delayed_render_timer.upper_fd != -1)
        close(term->delayed_render_timer.upper_fd);

    mtx_lock(&term->render.workers.lock);
    assert(tll_length(term->render.workers.queue) == 0);
    for (size_t i = 0; i < term->render.workers.count; i++) {
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

    if (term->flash.fd != -1)
        close(term->flash.fd);
    if (term->blink.fd != -1)
        close(term->blink.fd);

    if (term->ptmx != -1)
        close(term->ptmx);

    for (size_t i = 0; i < term->render.workers.count; i++)
        thrd_join(term->render.workers.threads[i], NULL);
    free(term->render.workers.threads);
    cnd_destroy(&term->render.workers.cond);
    mtx_destroy(&term->render.workers.lock);
    sem_destroy(&term->render.workers.start);
    sem_destroy(&term->render.workers.done);
    assert(tll_length(term->render.workers.queue) == 0);
    tll_free(term->render.workers.queue);

    int ret = EXIT_SUCCESS;

    if (term->slave > 0) {
        /* Note: we've closed ptmx, so the slave *should* exit... */
        int status;
        waitpid(term->slave, &status, 0);

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

        ret = child_ret;
    }

    free(term);
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
    term->selected_charset = 0;
    term->charset[0] = CHARSET_ASCII;
    term->charset[1] = CHARSET_ASCII;
    term->charset[2] = CHARSET_ASCII;
    term->charset[3] = CHARSET_ASCII;
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
    term->print_needs_wrap = false;
    term->cursor = (struct coord){0, 0};
    term->saved_cursor = (struct coord){0, 0};
    term->alt_saved_cursor = (struct coord){0, 0};
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

void
term_cursor_to(struct terminal *term, int row, int col)
{
    assert(row < term->rows);
    assert(col < term->cols);

    term->print_needs_wrap = false;

    term->cursor.col = col;
    term->cursor.row = row;

    term->grid->cur_row = grid_row(term->grid, row);
}

void
term_cursor_left(struct terminal *term, int count)
{
    int move_amount = min(term->cursor.col, count);
    term->cursor.col -= move_amount;
    assert(term->cursor.col >= 0);
    term->print_needs_wrap = false;
}

void
term_cursor_right(struct terminal *term, int count)
{
    int move_amount = min(term->cols - term->cursor.col - 1, count);
    term->cursor.col += move_amount;
    assert(term->cursor.col < term->cols);
    term->print_needs_wrap = false;
}

void
term_cursor_up(struct terminal *term, int count)
{
    int move_amount = min(term->cursor.row, count);
    term_cursor_to(term, term->cursor.row - move_amount, term->cursor.col);
}

void
term_cursor_down(struct terminal *term, int count)
{
    int move_amount = min(term->rows - term->cursor.row - 1, count);
    term_cursor_to(term, term->cursor.row + move_amount, term->cursor.col);
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
    term->grid->cur_row = grid_row(term->grid, term->cursor.row);
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
    term->grid->cur_row = grid_row(term->grid, term->cursor.row);
}

void
term_scroll_reverse(struct terminal *term, int rows)
{
    term_scroll_reverse_partial(term, term->scroll_region, rows);
}

void
term_linefeed(struct terminal *term)
{
    if (term->cursor.row == term->scroll_region.end - 1)
        term_scroll(term, 1);
    else
        term_cursor_down(term, 1);
}

void
term_reverse_index(struct terminal *term)
{
    if (term->cursor.row == term->scroll_region.start)
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
    int row = min(term->saved_cursor.row, term->rows - 1);
    int col = min(term->saved_cursor.col, term->cols - 1);
    term_cursor_to(term, row, col);
}

void
term_focus_in(struct terminal *term)
{
    if (!term->focus_events)
        return;
    vt_to_slave(term, "\033[I", 3);
}

void
term_focus_out(struct terminal *term)
{
    if (!term->focus_events)
        return;
    vt_to_slave(term, "\033[O", 3);
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
    case MOUSE_NORMAL:
        snprintf(response, sizeof(response), "\033[M%c%c%c",
                 32 + (release ? 3 : encoded_button), 32 + col + 1, 32 + row + 1);
        break;

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

    vt_to_slave(term, response, strlen(response));
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

    case MOUSE_X10:
    case MOUSE_CLICK:
    case MOUSE_DRAG:
    case MOUSE_MOTION:
        report_mouse_click(term, encoded, row, col, false);
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

    case MOUSE_X10:
    case MOUSE_CLICK:
    case MOUSE_DRAG:
    case MOUSE_MOTION:
        report_mouse_click(term, encoded, row, col, true);
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
    case MOUSE_X10:
    case MOUSE_CLICK:
        return;

    case MOUSE_DRAG:
        if (button == 0)
            return;
        /* FALLTHROUGH */

    case MOUSE_MOTION:
        report_mouse_motion(term, encoded, row, col);
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
