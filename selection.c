#include "selection.h"

#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <wctype.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>

#define LOG_MODULE "selection"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "async.h"
#include "commands.h"
#include "config.h"
#include "extract.h"
#include "grid.h"
#include "misc.h"
#include "render.h"
#include "util.h"
#include "vt.h"
#include "xmalloc.h"

bool
selection_enabled(const struct terminal *term, struct seat *seat)
{
    return
        seat->mouse.col >= 0 && seat->mouse.row >= 0 &&
        (term->mouse_tracking == MOUSE_NONE ||
         term_mouse_grabbed(term, seat) ||
         term->is_searching);
}

bool
selection_on_rows(const struct terminal *term, int row_start, int row_end)
{
    LOG_DBG("on rows: %d-%d, range: %d-%d (offset=%d)",
            term->selection.start.row, term->selection.end.row,
            row_start, row_end, term->grid->offset);

    if (term->selection.end.row < 0)
        return false;

    assert(term->selection.start.row != -1);

    row_start += term->grid->offset;
    row_end += term->grid->offset;

    const struct coord *start = &term->selection.start;
    const struct coord *end = &term->selection.end;

    if ((row_start <= start->row && row_end >= start->row) ||
        (row_start <= end->row && row_end >= end->row))
    {
        /* The range crosses one of the selection boundaries */
        return true;
    }

    /* For the last check we must ensure start <= end */
    if (start->row > end->row) {
        const struct coord *tmp = start;
        start = end;
        end = tmp;
    }

    if (row_start >= start->row && row_end <= end->row) {
        LOG_INFO("ON ROWS");
        return true;
    }

    return false;
}

void
selection_view_up(struct terminal *term, int new_view)
{
    if (likely(term->selection.start.row < 0))
        return;

    if (likely(new_view < term->grid->view))
        return;

    term->selection.start.row += term->grid->num_rows;
    if (term->selection.end.row >= 0)
        term->selection.end.row += term->grid->num_rows;
}

void
selection_view_down(struct terminal *term, int new_view)
{
    if (likely(term->selection.start.row < 0))
        return;

    if (likely(new_view > term->grid->view))
        return;

    term->selection.start.row &= term->grid->num_rows - 1;
    if (term->selection.end.row >= 0)
        term->selection.end.row &= term->grid->num_rows - 1;
}

static void
foreach_selected_normal(
    struct terminal *term, struct coord _start, struct coord _end,
    bool (*cb)(struct terminal *term, struct row *row, struct cell *cell, int col, void *data),
    void *data)
{
    const struct coord *start = &_start;
    const struct coord *end = &_end;

    int start_row, end_row;
    int start_col, end_col;

    if (start->row < end->row) {
        start_row = start->row;
        end_row = end->row;
        start_col = start->col;
        end_col = end->col;
    } else if (start->row > end->row) {
        start_row = end->row;
        end_row = start->row;
        start_col = end->col;
        end_col = start->col;
    } else {
        start_row = end_row = start->row;
        start_col = min(start->col, end->col);
        end_col = max(start->col, end->col);
    }

    for (int r = start_row; r <= end_row; r++) {
        size_t real_r = r & (term->grid->num_rows - 1);
        struct row *row = term->grid->rows[real_r];
        assert(row != NULL);

        for (int c = start_col;
             c <= (r == end_row ? end_col : term->cols - 1);
             c++)
        {
            if (!cb(term, row, &row->cells[c], c, data))
                return;
        }

        start_col = 0;
    }
}

static void
foreach_selected_block(
    struct terminal *term, struct coord _start, struct coord _end,
    bool (*cb)(struct terminal *term, struct row *row, struct cell *cell, int col, void *data),
    void *data)
{
    const struct coord *start = &_start;
    const struct coord *end = &_end;

    struct coord top_left = {
        .row = min(start->row, end->row),
        .col = min(start->col, end->col),
    };

    struct coord bottom_right = {
        .row = max(start->row, end->row),
        .col = max(start->col, end->col),
    };

    for (int r = top_left.row; r <= bottom_right.row; r++) {
        size_t real_r = r & (term->grid->num_rows - 1);
        struct row *row = term->grid->rows[real_r];
        assert(row != NULL);

        for (int c = top_left.col; c <= bottom_right.col; c++) {
            if (!cb(term, row, &row->cells[c], c, data))
                return;
        }
    }
}

static void
foreach_selected(
    struct terminal *term, struct coord start, struct coord end,
    bool (*cb)(struct terminal *term, struct row *row, struct cell *cell, int col, void *data),
    void *data)
{
    switch (term->selection.kind) {
    case SELECTION_NORMAL:
        foreach_selected_normal(term, start, end, cb, data);
        return;

    case SELECTION_BLOCK:
        foreach_selected_block(term, start, end, cb, data);
        return;

    case SELECTION_NONE:
        assert(false);
        return;
    }

    assert(false);
}

static bool
extract_one_const_wrapper(struct terminal *term,
                          struct row *row, struct cell *cell,
                          int col, void *data)
{
    return extract_one(term, row, cell, col, data);
}

char *
selection_to_text(const struct terminal *term)
{
    if (term->selection.end.row == -1)
        return NULL;

    struct extraction_context *ctx = extract_begin(term->selection.kind);
    if (ctx == NULL)
        return NULL;

    foreach_selected(
        (struct terminal *)term, term->selection.start, term->selection.end,
        &extract_one_const_wrapper, ctx);

    char *text;
    return extract_finish(ctx, &text, NULL) ? text : NULL;
}

void
selection_start(struct terminal *term, int col, int row,
                enum selection_kind kind)
{
    selection_cancel(term);

    LOG_DBG("%s selection started at %d,%d",
            kind == SELECTION_NORMAL ? "normal" :
            kind == SELECTION_BLOCK ? "block" : "<unknown>",
            row, col);

    term->selection.kind = kind;
    term->selection.start = (struct coord){col, term->grid->view + row};
    term->selection.end = (struct coord){-1, -1};
    term->selection.ongoing = true;
}

/* Context used while (un)marking selected cells, to be able to
 * exclude empty cells */
struct mark_context {
    const struct row *last_row;
    int empty_count;
};

static bool
unmark_selected(struct terminal *term, struct row *row, struct cell *cell,
                int col, void *data)
{
    if (cell->attrs.selected == 0 || (cell->attrs.selected & 2)) {
        /* Ignore if already deselected, or if premarked for updated selection */
        return true;
    }

    row->dirty = true;
    cell->attrs.selected = 0;
    cell->attrs.clean = 0;
    return true;
}


static bool
premark_selected(struct terminal *term, struct row *row, struct cell *cell,
                 int col, void *data)
{
    struct mark_context *ctx = data;
    assert(ctx != NULL);

    if (ctx->last_row != row) {
        ctx->last_row = row;
        ctx->empty_count = 0;
    }

    if (cell->wc == 0 && term->selection.kind == SELECTION_NORMAL) {
        ctx->empty_count++;
        return true;
    }

    /* Tell unmark to leave this be */
    for (int i = 0; i < ctx->empty_count + 1; i++)
        row->cells[col - i].attrs.selected |= 2;

    return true;
}

static bool
mark_selected(struct terminal *term, struct row *row, struct cell *cell,
              int col, void *data)
{
    struct mark_context *ctx = data;
    assert(ctx != NULL);

    if (ctx->last_row != row) {
        ctx->last_row = row;
        ctx->empty_count = 0;
    }

    if (cell->wc == 0 && term->selection.kind == SELECTION_NORMAL) {
        ctx->empty_count++;
        return true;
    }

    for (int i = 0; i < ctx->empty_count + 1; i++) {
        struct cell *c = &row->cells[col - i];
        if (c->attrs.selected & 1)
            c->attrs.selected = 1; /* Clear the pre-mark bit */
        else {
            row->dirty = true;
            c->attrs.selected = 1;
            c->attrs.clean = 0;
        }
    }

    return true;
}

static void
selection_modify(struct terminal *term, struct coord start, struct coord end)
{
    assert(term->selection.start.row != -1);
    assert(start.row != -1 && start.col != -1);
    assert(end.row != -1 && end.col != -1);

    struct mark_context ctx = {0};

    /* Premark all cells that *will* be selected */
    foreach_selected(term, start, end, &premark_selected, &ctx);
    memset(&ctx, 0, sizeof(ctx));

    if (term->selection.end.row >= 0) {
        /* Unmark previous selection, ignoring cells that are part of
         * the new selection */
        foreach_selected(term, term->selection.start, term->selection.end,
                         &unmark_selected, &ctx);
        memset(&ctx, 0, sizeof(ctx));
    }

    term->selection.start = start;
    term->selection.end = end;

    /* Mark new selection */
    foreach_selected(term, start, end, &mark_selected, &ctx);
    render_refresh(term);
}

void
selection_update(struct terminal *term, int col, int row)
{
    if (term->selection.start.row < 0)
        return;

    if (!term->selection.ongoing)
        return;

    LOG_DBG("selection updated: start = %d,%d, end = %d,%d -> %d, %d",
            term->selection.start.row, term->selection.start.col,
            term->selection.end.row, term->selection.end.col,
            row, col);

    assert(term->grid->view + row != -1);

    struct coord new_start = term->selection.start;
    struct coord new_end = {col, term->grid->view + row};

    size_t start_row_idx = new_start.row & (term->grid->num_rows - 1);
    size_t end_row_idx = new_end.row & (term->grid->num_rows - 1);
    const struct row *row_start = term->grid->rows[start_row_idx];
    const struct row *row_end = term->grid->rows[end_row_idx];

    /* Adjust start point if the selection has changed 'direction' */
    if (!(new_end.row == new_start.row && new_end.col == new_start.col)) {
        enum selection_direction new_direction;

        if (new_end.row > new_start.row ||
            (new_end.row == new_start.row && new_end.col > new_start.col))
        {
            /* New end point is after the start point */
            new_direction = SELECTION_RIGHT;
        } else {
            /* The new end point is before the start point */
            new_direction = SELECTION_LEFT;
        }

        if (term->selection.direction != new_direction) {
            if (term->selection.direction != SELECTION_UNDIR) {
                if (new_direction == SELECTION_LEFT) {
                    bool keep_going = true;
                    while (keep_going) {
                        const wchar_t wc = row_start->cells[new_start.col].wc;
                        keep_going = wc == CELL_MULT_COL_SPACER;

                        new_start.col--;
                        if (new_start.col < 0) {
                            new_start.col = term->cols - 1;
                            new_start.row--;
                        }
                    }
                } else {
                    bool keep_going = true;
                    while (keep_going) {
                        const wchar_t wc = new_start.col < term->cols - 1
                            ? row_start->cells[new_start.col + 1].wc
                            : 0;

                        keep_going = wc == CELL_MULT_COL_SPACER;

                        new_start.col++;
                        if (new_start.col >= term->cols) {
                            new_start.col = 0;
                            new_start.row++;
                        }
                    }
                }
            }

            term->selection.direction = new_direction;
        }
    }

    /* If an end point is in the middle of a multi-column character,
     * expand the selection to cover the entire character */
    if (new_start.row < new_end.row ||
        (new_start.row == new_end.row && new_start.col <= new_end.col))
    {
        while (new_start.col >= 1 &&
               row_start->cells[new_start.col].wc == CELL_MULT_COL_SPACER)
            new_start.col--;
        while (new_end.col < term->cols - 1 &&
               row_end->cells[new_end.col + 1].wc == CELL_MULT_COL_SPACER)
            new_end.col++;
    } else {
        while (new_end.col >= 1 &&
               row_end->cells[new_end.col].wc == CELL_MULT_COL_SPACER)
            new_end.col--;
        while (new_start.col < term->cols - 1 &&
               row_start->cells[new_start.col + 1].wc == CELL_MULT_COL_SPACER)
            new_start.col++;
    }

    selection_modify(term, new_start, new_end);
}

void
selection_dirty_cells(struct terminal *term)
{
    if (term->selection.start.row < 0 || term->selection.end.row < 0)
        return;

    foreach_selected(
        term, term->selection.start, term->selection.end, &mark_selected,
        &(struct mark_context){0});
}

static void
selection_extend_normal(struct terminal *term, int col, int row, uint32_t serial)
{
    const struct coord *start = &term->selection.start;
    const struct coord *end = &term->selection.end;

    if (start->row > end->row ||
        (start->row == end->row && start->col > end->col))
    {
        const struct coord *tmp = start;
        start = end;
        end = tmp;
    }

    assert(start->row < end->row || start->col < end->col);

    struct coord new_start, new_end;

    if (row < start->row || (row == start->row && col < start->col)) {
        /* Extend selection to start *before* current start */
        new_start = *end;
        new_end = (struct coord){col, row};
    }

    else if (row > end->row || (row == end->row && col > end->col)) {
        /* Extend selection to end *after* current end */
        new_start = *start;
        new_end = (struct coord){col, row};
    }

    else {
        /* Shrink selection from start or end, depending on which one is closest */

        const int linear = row * term->cols + col;

        if (abs(linear - (start->row * term->cols + start->col)) <
            abs(linear - (end->row * term->cols + end->col)))
        {
            /* Move start point */
            new_start = *end;
            new_end = (struct coord){col, row};
        }

        else {
            /* Move end point */
            new_start = *start;
            new_end = (struct coord){col, row};
        }
    }

    selection_modify(term, new_start, new_end);
}

static void
selection_extend_block(struct terminal *term, int col, int row, uint32_t serial)
{
    const struct coord *start = &term->selection.start;
    const struct coord *end = &term->selection.end;

    struct coord top_left = {
        .row = min(start->row, end->row),
        .col = min(start->col, end->col),
    };

    struct coord top_right = {
        .row = min(start->row, end->row),
        .col = max(start->col, end->col),
    };

    struct coord bottom_left = {
        .row = max(start->row, end->row),
        .col = min(start->col, end->col),
    };

    struct coord bottom_right = {
        .row = max(start->row, end->row),
        .col = max(start->col, end->col),
    };

    struct coord new_start;
    struct coord new_end;

    if (row <= top_left.row ||
        abs(row - top_left.row) < abs(row - bottom_left.row))
    {
        /* Move one of the top corners */

        if (abs(col - top_left.col) < abs(col - top_right.col)) {
            new_start = bottom_right;
            new_end = (struct coord){col, row};
        }

        else {
            new_start = bottom_left;
            new_end = (struct coord){col, row};
        }
    }

    else {
        /* Move one of the bottom corners */

        if (abs(col - bottom_left.col) < abs(col - bottom_right.col)) {
            new_start = top_right;
            new_end = (struct coord){col, row};
        }

        else {
            new_start = top_left;
            new_end = (struct coord){col, row};
        }
    }

    selection_modify(term, new_start, new_end);
}

void
selection_extend(struct seat *seat, struct terminal *term,
                 int col, int row, uint32_t serial)
{
    if (term->selection.start.row < 0 || term->selection.end.row < 0) {
        /* No existing selection */
        return;
    }

    term->selection.ongoing = true;

    row += term->grid->view;

    if ((row == term->selection.start.row && col == term->selection.start.col) ||
        (row == term->selection.end.row && col == term->selection.end.col))
    {
        /* Extension point *is* one of the current end points */
        return;
    }

    switch (term->selection.kind) {
    case SELECTION_NONE:
        assert(false);
        return;

    case SELECTION_NORMAL:
        selection_extend_normal(term, col, row, serial);
        break;

    case SELECTION_BLOCK:
        selection_extend_block(term, col, row, serial);
        break;
    }

    selection_to_primary(seat, term, serial);
}

//static const struct zwp_primary_selection_source_v1_listener primary_selection_source_listener;

void
selection_finalize(struct seat *seat, struct terminal *term, uint32_t serial)
{
    if (!term->selection.ongoing)
        return;

    selection_stop_scroll_timer(term);
    term->selection.ongoing = false;

    if (term->selection.start.row < 0 || term->selection.end.row < 0)
        return;

    assert(term->selection.start.row != -1);
    assert(term->selection.end.row != -1);

    if (term->selection.start.row > term->selection.end.row ||
        (term->selection.start.row == term->selection.end.row &&
         term->selection.start.col > term->selection.end.col))
    {
        struct coord tmp = term->selection.start;
        term->selection.start = term->selection.end;
        term->selection.end = tmp;
    }

    assert(term->selection.start.row <= term->selection.end.row);
    selection_to_primary(seat, term, serial);
}

void
selection_cancel(struct terminal *term)
{
    LOG_DBG("selection cancelled: start = %d,%d end = %d,%d",
            term->selection.start.row, term->selection.start.col,
            term->selection.end.row, term->selection.end.col);

    if (term->selection.auto_scroll.fd >= 0) {
        fdm_del(term->fdm, term->selection.auto_scroll.fd);
        term->selection.auto_scroll.fd = -1;
    }

    if (term->selection.start.row >= 0 && term->selection.end.row >= 0) {
        foreach_selected(
            term, term->selection.start, term->selection.end,
            &unmark_selected, &(struct mark_context){0});
        render_refresh(term);
    }

    term->selection.kind = SELECTION_NONE;
    term->selection.start = (struct coord){-1, -1};
    term->selection.end = (struct coord){-1, -1};
    term->selection.direction = SELECTION_UNDIR;
    term->selection.ongoing = false;
}

bool
selection_clipboard_has_data(const struct seat *seat)
{
    return seat->clipboard.data_offer != NULL;
}

bool
selection_primary_has_data(const struct seat *seat)
{
    return seat->primary.data_offer != NULL;
}

void
selection_clipboard_unset(struct seat *seat)
{
    struct wl_clipboard *clipboard = &seat->clipboard;

    if (clipboard->data_source == NULL)
        return;

    /* Kill previous data source */
    assert(clipboard->serial != 0);
    wl_data_device_set_selection(seat->data_device, NULL, clipboard->serial);
    wl_data_source_destroy(clipboard->data_source);

    clipboard->data_source = NULL;
    clipboard->serial = 0;

    free(clipboard->text);
    clipboard->text = NULL;
}

void
selection_primary_unset(struct seat *seat)
{
    struct wl_primary *primary = &seat->primary;

    if (primary->data_source == NULL)
        return;

    assert(primary->serial != 0);
    zwp_primary_selection_device_v1_set_selection(
        seat->primary_selection_device, NULL, primary->serial);
    zwp_primary_selection_source_v1_destroy(primary->data_source);

    primary->data_source = NULL;
    primary->serial = 0;

    free(primary->text);
    primary->text = NULL;
}

void
selection_mark_word(struct seat *seat, struct terminal *term, int col, int row,
                    bool spaces_only, uint32_t serial)
{
    selection_cancel(term);

    struct coord start = {col, row};
    struct coord end = {col, row};

    const struct row *r = grid_row_in_view(term->grid, start.row);
    wchar_t c = r->cells[start.col].wc;

    if (!(c == 0 || !isword(c, spaces_only, term->conf->word_delimiters))) {
        while (true) {
            int next_col = start.col - 1;
            int next_row = start.row;

            /* Linewrap */
            if (next_col < 0) {
                next_col = term->cols - 1;
                if (--next_row < 0)
                    break;
            }

            const struct row *row = grid_row_in_view(term->grid, next_row);

            c = row->cells[next_col].wc;
            if (c == 0 || !isword(c, spaces_only, term->conf->word_delimiters))
                break;

            start.col = next_col;
            start.row = next_row;
        }
    }

    r = grid_row_in_view(term->grid, end.row);
    c = r->cells[end.col].wc;

    if (!(c == 0 || !isword(c, spaces_only, term->conf->word_delimiters))) {
        while (true) {
            int next_col = end.col + 1;
            int next_row = end.row;

            /* Linewrap */
            if (next_col >= term->cols) {
                next_col = 0;
                if (++next_row >= term->rows)
                    break;
            }

            const struct row *row = grid_row_in_view(term->grid, next_row);

            c = row->cells[next_col].wc;
            if (c == '\0' || !isword(c, spaces_only, term->conf->word_delimiters))
                break;

            end.col = next_col;
            end.row = next_row;
        }
    }

    selection_start(term, start.col, start.row, SELECTION_NORMAL);
    selection_update(term, end.col, end.row);
    selection_finalize(seat, term, serial);
}

void
selection_mark_row(
    struct seat *seat, struct terminal *term, int row, uint32_t serial)
{
    selection_start(term, 0, row, SELECTION_NORMAL);
    selection_update(term, term->cols - 1, row);
    selection_finalize(seat, term, serial);
}

static bool
fdm_scroll_timer(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct terminal *term = data;

    uint64_t expiration_count;
    ssize_t ret = read(
        term->selection.auto_scroll.fd,
        &expiration_count, sizeof(expiration_count));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read selection scroll timer");
        return false;
    }

    switch (term->selection.auto_scroll.direction) {
    case SELECTION_SCROLL_NOT:
        return true;

    case SELECTION_SCROLL_UP:
        for (uint64_t i = 0; i < expiration_count; i++)
            cmd_scrollback_up(term, 1);
        selection_update(term, term->selection.auto_scroll.col, 0);
        break;

    case SELECTION_SCROLL_DOWN:
        for (uint64_t i = 0; i < expiration_count; i++)
            cmd_scrollback_down(term, 1);
        selection_update(term, term->selection.auto_scroll.col, term->rows - 1);
        break;
    }


    return true;
}

void
selection_start_scroll_timer(struct terminal *term, int interval_ns,
                             enum selection_scroll_direction direction, int col)
{
    assert(direction != SELECTION_SCROLL_NOT);

    if (!term->selection.ongoing)
        return;

    if (term->selection.auto_scroll.fd < 0) {
        int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (fd < 0) {
            LOG_ERRNO("failed to create selection scroll timer");
            goto err;
        }

        if (!fdm_add(term->fdm, fd, EPOLLIN, &fdm_scroll_timer, term)) {
            close(fd);
            return;
        }

        term->selection.auto_scroll.fd = fd;
    }

    struct itimerspec timer;
    if (timerfd_gettime(term->selection.auto_scroll.fd, &timer) < 0) {
        LOG_ERRNO("failed to get current selection scroll timer value");
        goto err;
    }

    if (timer.it_value.tv_sec == 0 && timer.it_value.tv_nsec == 0)
        timer.it_value.tv_nsec = 1;

    timer.it_interval.tv_sec = interval_ns / 1000000000;
    timer.it_interval.tv_nsec = interval_ns % 1000000000;

    if (timerfd_settime(term->selection.auto_scroll.fd, 0, &timer, NULL) < 0) {
        LOG_ERRNO("failed to set new selection scroll timer value");
        goto err;
    }

    term->selection.auto_scroll.direction = direction;
    term->selection.auto_scroll.col = col;
    return;

err:
    selection_stop_scroll_timer(term);
    return;
}

void
selection_stop_scroll_timer(struct terminal *term)
{
    if (term->selection.auto_scroll.fd < 0) {
        assert(term->selection.auto_scroll.direction == SELECTION_SCROLL_NOT);
        return;
    }

    fdm_del(term->fdm, term->selection.auto_scroll.fd);
    term->selection.auto_scroll.fd = -1;
    term->selection.auto_scroll.direction = SELECTION_SCROLL_NOT;
}

static void
target(void *data, struct wl_data_source *wl_data_source, const char *mime_type)
{
    LOG_WARN("TARGET: mime-type=%s", mime_type);
}

struct clipboard_send {
    char *data;
    size_t len;
    size_t idx;
};

static bool
fdm_send(struct fdm *fdm, int fd, int events, void *data)
{
    struct clipboard_send *ctx = data;

    if (events & EPOLLHUP)
        goto done;

    switch (async_write(fd, ctx->data, ctx->len, &ctx->idx)) {
    case ASYNC_WRITE_REMAIN:
        return true;

    case ASYNC_WRITE_DONE:
        break;

    case ASYNC_WRITE_ERR:
        LOG_ERRNO(
            "failed to asynchronously write %zu of selection data to FD=%d",
            ctx->len - ctx->idx, fd);
        break;
    }

done:
    fdm_del(fdm, fd);
    free(ctx->data);
    free(ctx);
    return true;
}

static void
send(void *data, struct wl_data_source *wl_data_source, const char *mime_type,
     int32_t fd)
{
    struct seat *seat = data;
    const struct wl_clipboard *clipboard = &seat->clipboard;

    assert(clipboard != NULL);
    assert(clipboard->text != NULL);

    const char *selection = clipboard->text;
    const size_t len = strlen(selection);

    /* Make it NONBLOCK:ing right away - we don't want to block if the
     * initial attempt to send the data synchronously fails */
    int flags;
    if ((flags = fcntl(fd, F_GETFL)) < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        LOG_ERRNO("failed to set O_NONBLOCK");
        return;
    }

    size_t async_idx = 0;
    switch (async_write(fd, selection, len, &async_idx)) {
    case ASYNC_WRITE_REMAIN: {
        struct clipboard_send *ctx = xmalloc(sizeof(*ctx));
        *ctx = (struct clipboard_send) {
            .data = xstrdup(selection),
            .len = len,
            .idx = async_idx,
        };

        if (fdm_add(seat->wayl->fdm, fd, EPOLLOUT, &fdm_send, ctx))
            return;

        free(ctx->data);
        free(ctx);
        break;
    }

    case ASYNC_WRITE_DONE:
        break;

    case ASYNC_WRITE_ERR:
        LOG_ERRNO(
            "failed to write %zu bytes of clipboard selection data to FD=%d",
            len, fd);
        break;
    }

    close(fd);
}

static void
cancelled(void *data, struct wl_data_source *wl_data_source)
{
    struct seat *seat = data;
    struct wl_clipboard *clipboard = &seat->clipboard;
    assert(clipboard->data_source == wl_data_source);

    wl_data_source_destroy(clipboard->data_source);
    clipboard->data_source = NULL;
    clipboard->serial = 0;

    free(clipboard->text);
    clipboard->text = NULL;
}

static void
dnd_drop_performed(void *data, struct wl_data_source *wl_data_source)
{
}

static void
dnd_finished(void *data, struct wl_data_source *wl_data_source)
{
}

static void
action(void *data, struct wl_data_source *wl_data_source, uint32_t dnd_action)
{
}

static const struct wl_data_source_listener data_source_listener = {
    .target = &target,
    .send = &send,
    .cancelled = &cancelled,
    .dnd_drop_performed = &dnd_drop_performed,
    .dnd_finished = &dnd_finished,
    .action = &action,
};

static void
primary_send(void *data,
             struct zwp_primary_selection_source_v1 *zwp_primary_selection_source_v1,
             const char *mime_type, int32_t fd)
{
    struct seat *seat = data;
    const struct wl_primary *primary = &seat->primary;

    assert(primary != NULL);
    assert(primary->text != NULL);

    const char *selection = primary->text;
    const size_t len = strlen(selection);

    int flags;
    if ((flags = fcntl(fd, F_GETFL)) < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        LOG_ERRNO("failed to set O_NONBLOCK");
        return;
    }

    size_t async_idx = 0;
    switch (async_write(fd, selection, len, &async_idx)) {
    case ASYNC_WRITE_REMAIN: {
        struct clipboard_send *ctx = xmalloc(sizeof(*ctx));
        *ctx = (struct clipboard_send) {
            .data = xstrdup(selection),
            .len = len,
            .idx = async_idx,
        };

        if (fdm_add(seat->wayl->fdm, fd, EPOLLOUT, &fdm_send, ctx))
            return;

        free(ctx->data);
        free(ctx);
        break;
    }

    case ASYNC_WRITE_DONE:
        break;

    case ASYNC_WRITE_ERR:
        LOG_ERRNO(
            "failed to write %zu bytes of primary selection data to FD=%d",
            len, fd);
        break;
    }

    close(fd);
}

static void
primary_cancelled(void *data,
                  struct zwp_primary_selection_source_v1 *zwp_primary_selection_source_v1)
{
    struct seat *seat = data;
    struct wl_primary *primary = &seat->primary;

    zwp_primary_selection_source_v1_destroy(primary->data_source);
    primary->data_source = NULL;
    primary->serial = 0;

    free(primary->text);
    primary->text = NULL;
}

static const struct zwp_primary_selection_source_v1_listener primary_selection_source_listener = {
    .send = &primary_send,
    .cancelled = &primary_cancelled,
};

bool
text_to_clipboard(struct seat *seat, struct terminal *term, char *text, uint32_t serial)
{
    struct wl_clipboard *clipboard = &seat->clipboard;

    if (clipboard->data_source != NULL) {
        /* Kill previous data source */
        assert(clipboard->serial != 0);
        wl_data_device_set_selection(seat->data_device, NULL, clipboard->serial);
        wl_data_source_destroy(clipboard->data_source);
        free(clipboard->text);

        clipboard->data_source = NULL;
        clipboard->serial = 0;
        clipboard->text = NULL;
    }

    clipboard->data_source
        = wl_data_device_manager_create_data_source(term->wl->data_device_manager);

    if (clipboard->data_source == NULL) {
        LOG_ERR("failed to create clipboard data source");
        return false;
    }

    clipboard->text = text;

    /* Configure source */
    wl_data_source_offer(clipboard->data_source, "text/plain;charset=utf-8");
    wl_data_source_add_listener(clipboard->data_source, &data_source_listener, seat);
    wl_data_device_set_selection(seat->data_device, clipboard->data_source, serial);

    /* Needed when sending the selection to other client */
    assert(serial != 0);
    clipboard->serial = serial;
    return true;
}

void
selection_to_clipboard(struct seat *seat, struct terminal *term, uint32_t serial)
{
    if (term->selection.start.row < 0 || term->selection.end.row < 0)
        return;

    /* Get selection as a string */
    char *text = selection_to_text(term);
    if (!text_to_clipboard(seat, term, text, serial))
        free(text);
}

struct clipboard_receive {
    int read_fd;
    int timeout_fd;
    struct itimerspec timeout;

    /* Callback data */
    void (*cb)(const char *data, size_t size, void *user);
    void (*done)(void *user);
    void *user;
};

static void
clipboard_receive_done(struct fdm *fdm, struct clipboard_receive *ctx)
{
    fdm_del(fdm, ctx->timeout_fd);
    fdm_del(fdm, ctx->read_fd);
    ctx->done(ctx->user);
    free(ctx);
}

static bool
fdm_receive_timeout(struct fdm *fdm, int fd, int events, void *data)
{
    struct clipboard_receive *ctx = data;
    if (events & EPOLLHUP)
        return false;

    assert(events & EPOLLIN);

    uint64_t expire_count;
    ssize_t ret = read(fd, &expire_count, sizeof(expire_count));
    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read clipboard timeout timer");
        return false;
    }

    LOG_WARN("no data received from clipboard in %llu seconds, aborting",
             (unsigned long long)ctx->timeout.it_value.tv_sec);

    clipboard_receive_done(fdm, ctx);
    return true;
}

static bool
fdm_receive(struct fdm *fdm, int fd, int events, void *data)
{
    struct clipboard_receive *ctx = data;

    if ((events & EPOLLHUP) && !(events & EPOLLIN))
        goto done;

    /* Reset timeout timer */
    if (timerfd_settime(ctx->timeout_fd, 0, &ctx->timeout, NULL) < 0) {
        LOG_ERRNO("failed to re-arm clipboard timeout timer");
        return false;
    }

    /* Read until EOF */
    while (true) {
        char text[256];
        ssize_t count = read(fd, text, sizeof(text));

        if (count == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;

            LOG_ERRNO("failed to read clipboard data");
            break;
        }

        if (count == 0)
            break;

        /* Call cb while at same time replacing \r\n with \n */
        const char *p = text;
        size_t left = count;
    again:
        for (size_t i = 0; i < left - 1; i++) {
            if (p[i] == '\r' && p[i + 1] == '\n') {
                ctx->cb(p, i, ctx->user);

                assert(i + 1 <= left);
                p += i + 1;
                left -= i + 1;
                goto again;
            }
        }

        ctx->cb(p, left, ctx->user);
        left = 0;
    }

done:
    clipboard_receive_done(fdm, ctx);
    return true;
}

static void
begin_receive_clipboard(struct terminal *term, int read_fd,
                        void (*cb)(const char *data, size_t size, void *user),
                        void (*done)(void *user), void *user)
{
    int timeout_fd = -1;
    struct clipboard_receive *ctx = NULL;

    int flags;
    if ((flags = fcntl(read_fd, F_GETFL)) < 0 ||
        fcntl(read_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        LOG_ERRNO("failed to set O_NONBLOCK");
        goto err;
    }

    timeout_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timeout_fd < 0) {
        LOG_ERRNO("failed to create clipboard timeout timer FD");
        goto err;
    }

    const struct itimerspec timeout = {.it_value = {.tv_sec = 2}};
    if (timerfd_settime(timeout_fd, 0, &timeout, NULL) < 0) {
        LOG_ERRNO("failed to arm clipboard timeout timer");
        goto err;
    }

    ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct clipboard_receive) {
        .read_fd = read_fd,
        .timeout_fd = timeout_fd,
        .timeout = timeout,
        .cb = cb,
        .done = done,
        .user = user,
    };

    if (!fdm_add(term->fdm, read_fd, EPOLLIN, &fdm_receive, ctx) ||
        !fdm_add(term->fdm, timeout_fd, EPOLLIN, &fdm_receive_timeout, ctx))
    {
        goto err;
    }

    return;

err:
    free(ctx);
    fdm_del(term->fdm, timeout_fd);
    fdm_del(term->fdm, read_fd);
    done(user);
}

void
text_from_clipboard(struct seat *seat, struct terminal *term,
                    void (*cb)(const char *data, size_t size, void *user),
                    void (*done)(void *user), void *user)
{
    struct wl_clipboard *clipboard = &seat->clipboard;
    if (clipboard->data_offer == NULL) {
        done(user);
        return;
    }

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        done(user);
        return;
    }

    int read_fd = fds[0];
    int write_fd = fds[1];

    /* Give write-end of pipe to other client */
    wl_data_offer_receive(
        clipboard->data_offer, "text/plain;charset=utf-8", write_fd);

    /* Don't keep our copy of the write-end open (or we'll never get EOF) */
    close(write_fd);

    begin_receive_clipboard(term, read_fd, cb, done, user);
}

static void
from_clipboard_cb(const char *data, size_t size, void *user)
{
    struct terminal *term = user;
    assert(term->is_sending_paste_data);
    term_paste_data_to_slave(term, data, size);
}

static void
from_clipboard_done(void *user)
{
    struct terminal *term = user;

    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[201~", 6);

    term->is_sending_paste_data = false;

    /* Make sure we send any queued up non-paste data */
    if (tll_length(term->ptmx_buffers) > 0)
        fdm_event_add(term->fdm, term->ptmx, EPOLLOUT);
}

void
selection_from_clipboard(struct seat *seat, struct terminal *term, uint32_t serial)
{
    if (term->is_sending_paste_data) {
        /* We're already pasting... */
        return;
    }

    struct wl_clipboard *clipboard = &seat->clipboard;
    if (clipboard->data_offer == NULL)
        return;

    term->is_sending_paste_data = true;

    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[200~", 6);

    text_from_clipboard(
        seat, term, &from_clipboard_cb, &from_clipboard_done, term);
}

bool
text_to_primary(struct seat *seat, struct terminal *term, char *text, uint32_t serial)
{
    if (term->wl->primary_selection_device_manager == NULL)
        return false;

    struct wl_primary *primary = &seat->primary;

    /* TODO: somehow share code with the clipboard equivalent */
    if (seat->primary.data_source != NULL) {
        /* Kill previous data source */

        assert(primary->serial != 0);
        zwp_primary_selection_device_v1_set_selection(
            seat->primary_selection_device, NULL, primary->serial);
        zwp_primary_selection_source_v1_destroy(primary->data_source);
        free(primary->text);

        primary->data_source = NULL;
        primary->serial = 0;
        primary->text = NULL;
    }

    primary->data_source
        = zwp_primary_selection_device_manager_v1_create_source(
            term->wl->primary_selection_device_manager);

    if (primary->data_source == NULL) {
        LOG_ERR("failed to create clipboard data source");
        return false;
    }

    /* Get selection as a string */
    primary->text = text;

    /* Configure source */
    zwp_primary_selection_source_v1_offer(primary->data_source, "text/plain;charset=utf-8");
    zwp_primary_selection_source_v1_add_listener(primary->data_source, &primary_selection_source_listener, seat);
    zwp_primary_selection_device_v1_set_selection(seat->primary_selection_device, primary->data_source, serial);

    /* Needed when sending the selection to other client */
    primary->serial = serial;
    return true;
}

void
selection_to_primary(struct seat *seat, struct terminal *term, uint32_t serial)
{
    if (term->wl->primary_selection_device_manager == NULL)
        return;

    /* Get selection as a string */
    char *text = selection_to_text(term);
    if (!text_to_primary(seat, term, text, serial))
        free(text);
}

void
text_from_primary(
    struct seat *seat, struct terminal *term,
    void (*cb)(const char *data, size_t size, void *user),
    void (*done)(void *user), void *user)
{
    if (term->wl->primary_selection_device_manager == NULL) {
        done(user);
        return;
    }

    struct wl_primary *primary = &seat->primary;
    if (primary->data_offer == NULL){
        done(user);
        return;
    }

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        done(user);
        return;
    }

    int read_fd = fds[0];
    int write_fd = fds[1];

    /* Give write-end of pipe to other client */
    zwp_primary_selection_offer_v1_receive(
        primary->data_offer, "text/plain;charset=utf-8", write_fd);

    /* Don't keep our copy of the write-end open (or we'll never get EOF) */
    close(write_fd);

    begin_receive_clipboard(term, read_fd, cb, done, user);
}

void
selection_from_primary(struct seat *seat, struct terminal *term)
{
    if (term->wl->primary_selection_device_manager == NULL)
        return;

    if (term->is_sending_paste_data) {
        /* We're already pasting... */
        return;
    }

    struct wl_primary *primary = &seat->primary;
    if (primary->data_offer == NULL)
        return;

    term->is_sending_paste_data = true;
    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[200~", 6);

    text_from_primary(seat, term, &from_clipboard_cb, &from_clipboard_done, term);
}

#if 0
static void
offer(void *data, struct wl_data_offer *wl_data_offer, const char *mime_type)
{
}

static void
source_actions(void *data, struct wl_data_offer *wl_data_offer,
                uint32_t source_actions)
{
}

static void
offer_action(void *data, struct wl_data_offer *wl_data_offer, uint32_t dnd_action)
{
}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer = &offer,
    .source_actions = &source_actions,
    .action = &offer_action,
};
#endif

static void
data_offer(void *data, struct wl_data_device *wl_data_device,
           struct wl_data_offer *id)
{
}

static void
enter(void *data, struct wl_data_device *wl_data_device, uint32_t serial,
      struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
      struct wl_data_offer *id)
{
}

static void
leave(void *data, struct wl_data_device *wl_data_device)
{
}

static void
motion(void *data, struct wl_data_device *wl_data_device, uint32_t time,
       wl_fixed_t x, wl_fixed_t y)
{
}

static void
drop(void *data, struct wl_data_device *wl_data_device)
{
}

static void
selection(void *data, struct wl_data_device *wl_data_device,
          struct wl_data_offer *id)
{
    /* Selection offer from other client */

    struct seat *seat = data;
    struct wl_clipboard *clipboard = &seat->clipboard;

    if (clipboard->data_offer != NULL)
        wl_data_offer_destroy(clipboard->data_offer);

    clipboard->data_offer = id;
#if 0
    if (id != NULL)
        wl_data_offer_add_listener(id, &data_offer_listener, term);
#endif
}

const struct wl_data_device_listener data_device_listener = {
    .data_offer = &data_offer,
    .enter = &enter,
    .leave = &leave,
    .motion = &motion,
    .drop = &drop,
    .selection = &selection,
};

#if 0
static void
primary_offer(void *data,
              struct zwp_primary_selection_offer_v1 *zwp_primary_selection_offer,
              const char *mime_type)
{
}

static const struct zwp_primary_selection_offer_v1_listener primary_selection_offer_listener = {
    .offer = &primary_offer,
};
#endif

static void
primary_data_offer(void *data,
                   struct zwp_primary_selection_device_v1 *zwp_primary_selection_device,
                   struct zwp_primary_selection_offer_v1 *offer)
{
}

static void
primary_selection(void *data,
                  struct zwp_primary_selection_device_v1 *zwp_primary_selection_device,
                  struct zwp_primary_selection_offer_v1 *id)
{
    /* Selection offer from other client, for primary */

    struct seat *seat = data;
    struct wl_primary *primary = &seat->primary;

    if (primary->data_offer != NULL)
        zwp_primary_selection_offer_v1_destroy(primary->data_offer);

    primary->data_offer = id;
#if 0
    if (id != NULL) {
        zwp_primary_selection_offer_v1_add_listener(
            id, &primary_selection_offer_listener, term);
    }
#endif
}

const struct zwp_primary_selection_device_v1_listener primary_selection_device_listener = {
    .data_offer = &primary_data_offer,
    .selection = &primary_selection,
};
