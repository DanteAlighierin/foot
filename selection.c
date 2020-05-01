#include "selection.h"

#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <wctype.h>

#include <sys/epoll.h>

#define LOG_MODULE "selection"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "async.h"
#include "grid.h"
#include "misc.h"
#include "render.h"
#include "util.h"
#include "vt.h"

bool
selection_enabled(const struct terminal *term)
{
    return
        term->mouse_tracking == MOUSE_NONE ||
        term_mouse_grabbed(term) ||
        term->is_searching;
}

bool
selection_on_row_in_view(const struct terminal *term, int row_no)
{
    if (term->selection.start.row == -1 || term->selection.end.row == -1)
        return false;

    const struct coord *start = &term->selection.start;
    const struct coord *end = &term->selection.end;
    assert(start->row <= end->row);

    row_no += term->grid->view;
    return row_no >= start->row && row_no <= end->row;
}

static void
foreach_selected_normal(
    struct terminal *term, struct coord _start, struct coord _end,
    void (*cb)(struct terminal *term, struct row *row, struct cell *cell, int col, void *data),
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
            cb(term, row, &row->cells[c], c, data);
        }

        start_col = 0;
    }
}

static void
foreach_selected_block(
    struct terminal *term, struct coord _start, struct coord _end,
    void (*cb)(struct terminal *term, struct row *row, struct cell *cell, int col, void *data),
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

        for (int c = top_left.col; c <= bottom_right.col; c++)
            cb(term, row, &row->cells[c], c, data);
    }
}

static void
foreach_selected(
    struct terminal *term, struct coord start, struct coord end,
    void (*cb)(struct terminal *term, struct row *row, struct cell *cell, int col, void *data),
    void *data)
{
    switch (term->selection.kind) {
    case SELECTION_NORMAL:
        return foreach_selected_normal(term, start, end, cb, data);

    case SELECTION_BLOCK:
        return foreach_selected_block(term, start, end, cb, data);

    case SELECTION_NONE:
        assert(false);
        return;
    }

    assert(false);
}

static size_t
min_bufsize_for_extraction(const struct terminal *term)
{
    const struct coord *start = &term->selection.start;
    const struct coord *end = &term->selection.end;
    const size_t chars_per_cell
        = 1 + ALEN(term->grid->cur_row->comb_chars[0].chars);

    switch (term->selection.kind) {
    case SELECTION_NONE:
        return 0;

    case SELECTION_NORMAL:
        if (term->selection.end.row == -1)
            return 0;

        assert(term->selection.start.row != -1);

        if (start->row > end->row) {
            const struct coord *tmp = start;
            start = end;
            end = tmp;
        }

        if (start->row == end->row)
            return (end->col - start->col + 1) * chars_per_cell;
        else {
            size_t cells = 0;

            /* Add one extra column on each row, for \n */

            cells += term->cols - start->col + 1;
            cells += (term->cols + 1) * (end->row - start->row - 1);
            cells += end->col + 1 + 1;
            return cells * chars_per_cell;
        }

    case SELECTION_BLOCK: {
        struct coord top_left = {
            .row = min(start->row, end->row),
            .col = min(start->col, end->col),
        };

        struct coord bottom_right = {
            .row = max(start->row, end->row),
            .col = max(start->col, end->col),
        };

        /* Add one extra column on each row, for \n */
        int cols = bottom_right.col - top_left.col + 1 + 1;
        int rows = bottom_right.row - top_left.row + 1;
        return rows * cols * chars_per_cell;
    }
    }

    assert(false);
    return 0;
}

struct extract {
    wchar_t *buf;
    size_t size;
    size_t idx;
    size_t empty_count;
    const struct row *last_row;
    const struct cell *last_cell;
};

static void
extract_one(struct terminal *term, struct row *row, struct cell *cell,
            int col, void *data)
{
    struct extract *ctx = data;

    if (ctx->last_row != NULL && row != ctx->last_row &&
        ((term->selection.kind == SELECTION_NORMAL &&
          ctx->last_row->linebreak) ||
         term->selection.kind == SELECTION_BLOCK))
    {
        /* Last cell was the last column in the selection */
        ctx->buf[ctx->idx++] = L'\n';
        ctx->empty_count = 0;
    }

    if (cell->wc == 0) {
        ctx->empty_count++;
        ctx->last_row = row;
        ctx->last_cell = cell;
        return;
    }

    /* Replace empty cells with spaces when followed by non-empty cell */
    assert(ctx->idx + ctx->empty_count <= ctx->size);
    for (size_t i = 0; i < ctx->empty_count; i++)
        ctx->buf[ctx->idx++] = L' ';
    ctx->empty_count = 0;

    assert(ctx->idx + 1 <= ctx->size);
    ctx->buf[ctx->idx++] = cell->wc;

    const struct combining_chars *comb_chars = &row->comb_chars[col];

    assert(cell->wc != 0);
    assert(ctx->idx + comb_chars->count <= ctx->size);
    for (size_t i = 0; i < comb_chars->count; i++)
        ctx->buf[ctx->idx++] = comb_chars->chars[i];

    ctx->last_row = row;
    ctx->last_cell = cell;
}

static char *
extract_selection(const struct terminal *term)
{
    const size_t max_cells = min_bufsize_for_extraction(term);
    const size_t buf_size = max_cells + 1;

    struct extract ctx = {
        .buf = malloc(buf_size * sizeof(wchar_t)),
        .size = buf_size,
    };

    foreach_selected(
        (struct terminal *)term, term->selection.start, term->selection.end,
        &extract_one, &ctx);

    if (ctx.idx == 0) {
        /* Selection of empty cells only */
        ctx.buf[ctx.idx] = L'\0';
    } else {
        assert(ctx.idx > 0);
        assert(ctx.idx < ctx.size);
        if (ctx.buf[ctx.idx - 1] == L'\n')
            ctx.buf[ctx.idx - 1] = L'\0';
        else
            ctx.buf[ctx.idx] = L'\0';
    }

    size_t len = wcstombs(NULL, ctx.buf, 0);
    if (len == (size_t)-1) {
        LOG_ERRNO("failed to convert selection to UTF-8");
        free(ctx.buf);
        return NULL;
    }

    char *ret = malloc(len + 1);
    wcstombs(ret, ctx.buf, len + 1);
    free(ctx.buf);
    return ret;
}

void
selection_start(struct terminal *term, int col, int row,
                enum selection_kind kind)
{
    if (!selection_enabled(term))
        return;

    selection_cancel(term);

    LOG_DBG("%s selection started at %d,%d",
            kind == SELECTION_NORMAL ? "normal" :
            kind == SELECTION_BLOCK ? "block" : "<unknown>",
            row, col);
    term->selection.kind = kind;
    term->selection.start = (struct coord){col, term->grid->view + row};
    term->selection.end = (struct coord){-1, -1};
}

static void
unmark_selected(struct terminal *term, struct row *row, struct cell *cell,
                int col, void *data)
{
    if (cell->attrs.selected == 0 || (cell->attrs.selected & 2)) {
        /* Ignore if already deselected, or if premarked for updated selection */
        return;
    }

    row->dirty = 1;
    cell->attrs.selected = 0;
    cell->attrs.clean = 0;
}

static void
premark_selected(struct terminal *term, struct row *row, struct cell *cell,
                 int col, void *data)
{
    /* Tell unmark to leave this be */
    cell->attrs.selected |= 2;
}

static void
mark_selected(struct terminal *term, struct row *row, struct cell *cell,
              int col, void *data)
{
    if (cell->attrs.selected & 1) {
        cell->attrs.selected = 1;  /* Clear the pre-mark bit */
        return;
    }

    row->dirty = 1;
    cell->attrs.selected = 1;
    cell->attrs.clean = 0;
}

static void
selection_modify(struct terminal *term, struct coord start, struct coord end)
{
    assert(selection_enabled(term));
    assert(term->selection.start.row != -1);
    assert(start.row != -1 && start.col != -1);
    assert(end.row != -1 && end.col != -1);

    /* Premark all cells that *will* be selected */
    foreach_selected(term, start, end, &premark_selected, NULL);

    if (term->selection.end.row != -1) {
        /* Unmark previous selection, ignoring cells that are part of
         * the new selection */
        foreach_selected(term, term->selection.start, term->selection.end,
                         &unmark_selected, NULL);
    }

    term->selection.start = start;
    term->selection.end = end;

    /* Mark new selection */
    foreach_selected(term, start, end, &mark_selected, NULL);
    render_refresh(term);
}

void
selection_update(struct terminal *term, int col, int row)
{
    if (!selection_enabled(term))
        return;

    LOG_DBG("selection updated: start = %d,%d, end = %d,%d -> %d, %d",
            term->selection.start.row, term->selection.start.col,
            term->selection.end.row, term->selection.end.col,
            row, col);

    assert(term->selection.start.row != -1);
    assert(term->grid->view + row != -1);

    struct coord new_end = {col, term->grid->view + row};
    selection_modify(term, term->selection.start, new_end);
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
        new_start = (struct coord){col, row};
        new_end = *end;
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
            new_start = (struct coord){col, row};
            new_end = *end;
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
            new_start = (struct coord){col, row};
            new_end = bottom_right;
        }

        else {
            new_start = (struct coord){col, row};
            new_end = bottom_left;
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
selection_extend(struct terminal *term, int col, int row, uint32_t serial)
{
    if (!selection_enabled(term))
        return;

    if (term->selection.start.row == -1 || term->selection.end.row == -1) {
        /* No existing selection */
        return;
    }

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

    selection_to_primary(term, serial);
}

static const struct zwp_primary_selection_source_v1_listener primary_selection_source_listener;

void
selection_finalize(struct terminal *term, uint32_t serial)
{
    if (term->selection.start.row == -1 || term->selection.end.row == -1)
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
    selection_to_primary(term, serial);
}

void
selection_cancel(struct terminal *term)
{
    if (!selection_enabled(term))
        return;

    LOG_DBG("selection cancelled: start = %d,%d end = %d,%d",
            term->selection.start.row, term->selection.start.col,
            term->selection.end.row, term->selection.end.col);

    if (term->selection.start.row != -1 && term->selection.end.row != -1) {
        foreach_selected(
            term, term->selection.start, term->selection.end,
            &unmark_selected, NULL);
        render_refresh(term);
    }

    term->selection.kind = SELECTION_NONE;
    term->selection.start = (struct coord){-1, -1};
    term->selection.end = (struct coord){-1, -1};
}

void
selection_mark_word(struct terminal *term, int col, int row, bool spaces_only,
                    uint32_t serial)
{
    if (!selection_enabled(term))
        return;

    selection_cancel(term);

    struct coord start = {col, row};
    struct coord end = {col, row};

    const struct row *r = grid_row_in_view(term->grid, start.row);
    wchar_t c = r->cells[start.col].wc;

    if (!(c == 0 || !isword(c, spaces_only))) {
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
            if (c == 0 || !isword(c, spaces_only))
                break;

            start.col = next_col;
            start.row = next_row;
        }
    }

    r = grid_row_in_view(term->grid, end.row);
    c = r->cells[end.col].wc;

    if (!(c == 0 || !isword(c, spaces_only))) {
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
            if (c == '\0' || !isword(c, spaces_only))
                break;

            end.col = next_col;
            end.row = next_row;
        }
    }

    selection_start(term, start.col, start.row, SELECTION_NORMAL);
    selection_update(term, end.col, end.row);
    selection_finalize(term, serial);
}

void
selection_mark_row(struct terminal *term, int row, uint32_t serial)
{
    selection_start(term, 0, row, SELECTION_NORMAL);
    selection_update(term, term->cols - 1, row);
    selection_finalize(term, serial);
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
    struct wayland *wayl = data;
    const struct wl_clipboard *clipboard = &wayl->clipboard;

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
        struct clipboard_send *ctx = malloc(sizeof(*ctx));
        *ctx = (struct clipboard_send) {
            .data = strdup(selection),
            .len = len,
            .idx = async_idx,
        };

        if (fdm_add(wayl->fdm, fd, EPOLLOUT, &fdm_send, ctx))
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
    struct wayland *wayl = data;
    struct wl_clipboard *clipboard = &wayl->clipboard;
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
    struct wayland *wayl = data;
    const struct wl_primary *primary = &wayl->primary;

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
        struct clipboard_send *ctx = malloc(sizeof(*ctx));
        *ctx = (struct clipboard_send) {
            .data = strdup(selection),
            .len = len,
            .idx = async_idx,
        };

        if (fdm_add(wayl->fdm, fd, EPOLLOUT, &fdm_send, ctx))
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
    struct wayland *wayl = data;
    struct wl_primary *primary = &wayl->primary;

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
text_to_clipboard(struct terminal *term, char *text, uint32_t serial)
{
    struct wl_clipboard *clipboard = &term->wl->clipboard;

    if (clipboard->data_source != NULL) {
        /* Kill previous data source */
        assert(clipboard->serial != 0);
        wl_data_device_set_selection(term->wl->data_device, NULL, clipboard->serial);
        wl_data_source_destroy(clipboard->data_source);
        free(clipboard->text);

        clipboard->data_source = NULL;
        clipboard->serial = 0;
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
    wl_data_source_add_listener(clipboard->data_source, &data_source_listener, term->wl);
    wl_data_device_set_selection(term->wl->data_device, clipboard->data_source, serial);

    /* Needed when sending the selection to other client */
    assert(serial != 0);
    clipboard->serial = serial;
    return true;
}

void
selection_to_clipboard(struct terminal *term, uint32_t serial)
{
    if (term->selection.start.row == -1 || term->selection.end.row == -1)
        return;

    /* Get selection as a string */
    char *text = extract_selection(term);
    if (!text_to_clipboard(term, text, serial))
        free(text);
}

struct clipboard_receive {
    /* Callback data */
    void (*cb)(const char *data, size_t size, void *user);
    void (*done)(void *user);
    void *user;
};

static bool
fdm_receive(struct fdm *fdm, int fd, int events, void *data)
{
    struct clipboard_receive *ctx = data;

    if ((events & EPOLLHUP) && !(events & EPOLLIN))
        goto done;

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
    fdm_del(fdm, fd);
    ctx->done(ctx->user);
    free(ctx);
    return true;
}

static void
begin_receive_clipboard(struct terminal *term, int read_fd,
                        void (*cb)(const char *data, size_t size, void *user),
                        void (*done)(void *user), void *user)
{
    int flags;
    if ((flags = fcntl(read_fd, F_GETFL)) < 0 ||
        fcntl(read_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        LOG_ERRNO("failed to set O_NONBLOCK");
        close(read_fd);
        return done(user);
    }

    struct clipboard_receive *ctx = malloc(sizeof(*ctx));
    *ctx = (struct clipboard_receive) {
        .cb = cb,
        .done = done,
        .user = user,
    };

    if (!fdm_add(term->fdm, read_fd, EPOLLIN, &fdm_receive, ctx)) {
        close(read_fd);
        free(ctx);
        done(user);
    }
}

void
text_from_clipboard(struct terminal *term, uint32_t serial,
                    void (*cb)(const char *data, size_t size, void *user),
                    void (*done)(void *user), void *user)
{
    struct wl_clipboard *clipboard = &term->wl->clipboard;
    if (clipboard->data_offer == NULL)
        return done(user);

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        return done(user);
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
    term_to_slave(term, data, size);
}

static void
from_clipboard_done(void *user)
{
    struct terminal *term = user;

    if (term->bracketed_paste)
        term_to_slave(term, "\033[201~", 6);
}

void
selection_from_clipboard(struct terminal *term, uint32_t serial)
{
    struct wl_clipboard *clipboard = &term->wl->clipboard;
    if (clipboard->data_offer == NULL)
        return;

    if (term->bracketed_paste)
        term_to_slave(term, "\033[200~", 6);

    text_from_clipboard(
        term, serial, &from_clipboard_cb, &from_clipboard_done, term);
}

bool
text_to_primary(struct terminal *term, char *text, uint32_t serial)
{
    if (term->wl->primary_selection_device_manager == NULL)
        return false;

    struct wl_primary *primary = &term->wl->primary;

    /* TODO: somehow share code with the clipboard equivalent */
    if (term->wl->primary.data_source != NULL) {
        /* Kill previous data source */

        assert(primary->serial != 0);
        zwp_primary_selection_device_v1_set_selection(
            term->wl->primary_selection_device, NULL, primary->serial);
        zwp_primary_selection_source_v1_destroy(primary->data_source);
        free(primary->text);

        primary->data_source = NULL;
        primary->serial = 0;
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
    zwp_primary_selection_source_v1_add_listener(primary->data_source, &primary_selection_source_listener, term->wl);
    zwp_primary_selection_device_v1_set_selection(term->wl->primary_selection_device, primary->data_source, serial);

    /* Needed when sending the selection to other client */
    primary->serial = serial;
    return true;
}

void
selection_to_primary(struct terminal *term, uint32_t serial)
{
    if (term->wl->primary_selection_device_manager == NULL)
        return;

    /* Get selection as a string */
    char *text = extract_selection(term);
    if (!text_to_primary(term, text, serial))
        free(text);
}

void
text_from_primary(
    struct terminal *term,
    void (*cb)(const char *data, size_t size, void *user),
    void (*done)(void *user), void *user)
{
    if (term->wl->primary_selection_device_manager == NULL)
        return done(user);

    struct wl_primary *primary = &term->wl->primary;
    if (primary->data_offer == NULL)
        return done(user);

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        return done(user);
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
selection_from_primary(struct terminal *term)
{
    if (term->wl->primary_selection_device_manager == NULL)
        return;

    struct wl_clipboard *clipboard = &term->wl->clipboard;
    if (clipboard->data_offer == NULL)
        return;

    if (term->bracketed_paste)
        term_to_slave(term, "\033[200~", 6);

    text_from_primary(term, &from_clipboard_cb, &from_clipboard_done, term);
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

    struct wayland *wayl = data;
    struct wl_clipboard *clipboard = &wayl->clipboard;

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

    struct wayland *wayl = data;
    struct wl_primary *primary = &wayl->primary;

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
