#include "grid.h"

#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "grid"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "macros.h"
#include "sixel.h"
#include "stride.h"
#include "util.h"
#include "xmalloc.h"

#define TIME_REFLOW 1

struct grid *
grid_snapshot(const struct grid *grid)
{
    struct grid *clone = xmalloc(sizeof(*clone));
    clone->num_rows = grid->num_rows;
    clone->num_cols = grid->num_cols;
    clone->offset = grid->offset;
    clone->view = grid->view;
    clone->cursor = grid->cursor;
    clone->rows = xcalloc(grid->num_rows, sizeof(clone->rows[0]));
    memset(&clone->scroll_damage, 0, sizeof(clone->scroll_damage));
    memset(&clone->sixel_images, 0, sizeof(clone->sixel_images));

    tll_foreach(grid->scroll_damage, it)
        tll_push_back(clone->scroll_damage, it->item);

    for (int r = 0; r < grid->num_rows; r++) {
        const struct row *row = grid->rows[r];

        if (row == NULL)
            continue;

        struct row *clone_row = xmalloc(sizeof(*row));
        clone->rows[r] = clone_row;

        clone_row->cells = xmalloc(grid->num_cols * sizeof(clone_row->cells[0]));
        clone_row->linebreak = row->linebreak;
        clone_row->dirty = row->dirty;

        for (int c = 0; c < grid->num_cols; c++)
            clone_row->cells[c] = row->cells[c];

        if (row->extra != NULL) {
            const struct row_data *extra = row->extra;
            struct row_data *new_extra = xcalloc(1, sizeof(*new_extra));

            tll_foreach(extra->uri_ranges, it) {
                struct row_uri_range range = {
                    .start = it->item.start,
                    .end = it->item.end,
                    .id = it->item.id,
                    .uri = xstrdup(it->item.uri),
                };

                tll_push_back(new_extra->uri_ranges, range);
            }

            clone_row->extra = new_extra;
        } else
            clone_row->extra = NULL;
    }

    tll_foreach(grid->sixel_images, it) {
        int width = it->item.width;
        int height = it->item.height;
        pixman_image_t *pix = it->item.pix;
        pixman_format_code_t pix_fmt = pixman_image_get_format(pix);
        int stride = stride_for_format_and_width(pix_fmt, width);

        size_t size = stride * height;
        void *new_data = xmalloc(size);
        memcpy(new_data, it->item.data, size);

        pixman_image_t *new_pix = pixman_image_create_bits_no_clear(
            pix_fmt, width, height, new_data, stride);

        struct sixel six = {
            .data = new_data,
            .pix = new_pix,
            .width = width,
            .height = height,
            .rows = it->item.rows,
            .cols = it->item.cols,
            .pos = it->item.pos,
        };

        tll_push_back(clone->sixel_images, six);
    }

    return clone;
}

void
grid_free(struct grid *grid)
{
    for (int r = 0; r < grid->num_rows; r++)
        grid_row_free(grid->rows[r]);

    tll_foreach(grid->sixel_images, it) {
        sixel_destroy(&it->item);
        tll_remove(grid->sixel_images, it);
    }

    free(grid->rows);
    tll_free(grid->scroll_damage);
}

void
grid_swap_row(struct grid *grid, int row_a, int row_b)
{
    xassert(grid->offset >= 0);
    xassert(row_a != row_b);

    int real_a = (grid->offset + row_a) & (grid->num_rows - 1);
    int real_b = (grid->offset + row_b) & (grid->num_rows - 1);

    struct row *a = grid->rows[real_a];
    struct row *b = grid->rows[real_b];

    grid->rows[real_a] = b;
    grid->rows[real_b] = a;
}

struct row *
grid_row_alloc(int cols, bool initialize)
{
    struct row *row = xmalloc(sizeof(*row));
    row->dirty = false;
    row->linebreak = false;
    row->extra = NULL;

    if (initialize) {
        row->cells = xcalloc(cols, sizeof(row->cells[0]));
        for (size_t c = 0; c < cols; c++)
            row->cells[c].attrs.clean = 1;
    } else
        row->cells = xmalloc(cols * sizeof(row->cells[0]));

    return row;
}

void
grid_row_free(struct row *row)
{
    if (row == NULL)
        return;

    grid_row_reset_extra(row);
    free(row->extra);
    free(row->cells);
    free(row);
}

void
grid_resize_without_reflow(
    struct grid *grid, int new_rows, int new_cols,
    int old_screen_rows, int new_screen_rows)
{
    struct row *const *old_grid = grid->rows;
    const int old_rows = grid->num_rows;
    const int old_cols = grid->num_cols;

    struct row **new_grid = xcalloc(new_rows, sizeof(new_grid[0]));

    tll(struct sixel) untranslated_sixels = tll_init();
    tll_foreach(grid->sixel_images, it)
        tll_push_back(untranslated_sixels, it->item);
    tll_free(grid->sixel_images);

    int new_offset = 0;

    /* Copy old lines, truncating them if old rows were longer */
    for (int r = 0, n = min(old_screen_rows, new_screen_rows); r < n; r++) {
        const int old_row_idx = (grid->offset + r) & (old_rows - 1);
        const int new_row_idx = (new_offset + r) & (new_rows - 1);

        const struct row *old_row = old_grid[old_row_idx];
        xassert(old_row != NULL);

        struct row *new_row = grid_row_alloc(new_cols, false);
        new_grid[new_row_idx] = new_row;

        memcpy(new_row->cells,
               old_row->cells,
               sizeof(struct cell) * min(old_cols, new_cols));

        new_row->dirty = old_row->dirty;
        new_row->linebreak = false;

        /* Clear "new" columns */
        if (new_cols > old_cols) {
            memset(&new_row->cells[old_cols], 0,
                   sizeof(struct cell) * (new_cols - old_cols));
            new_row->dirty = true;
        }

        /* Map sixels on current "old" row to current "new row" */
        tll_foreach(untranslated_sixels, it) {
            if (it->item.pos.row != old_row_idx)
                continue;

            struct sixel sixel = it->item;
            sixel.pos.row = new_row_idx;

            if (sixel.pos.col < new_cols)
                tll_push_back(grid->sixel_images, sixel);
            else
                sixel_destroy(&it->item);
            tll_remove(untranslated_sixels, it);
        }

        /* Copy URI ranges, truncating them if necessary */
        if (old_row->extra == NULL)
            continue;

        tll_foreach(old_row->extra->uri_ranges, it) {
            if (it->item.start >= new_rows) {
                /* The whole range is truncated */
                continue;
            }

            struct row_uri_range range = {
                .start = it->item.start,
                .end = min(it->item.end, new_cols - 1),
                .id = it->item.id,
                .uri = xstrdup(it->item.uri),
            };
            grid_row_add_uri_range(new_row, range);
        }
    }

    /* Clear "new" lines */
    for (int r = min(old_screen_rows, new_screen_rows); r < new_screen_rows; r++) {
        struct row *new_row = grid_row_alloc(new_cols, false);
        new_grid[(new_offset + r) & (new_rows - 1)] = new_row;

        memset(new_row->cells, 0, sizeof(struct cell) * new_cols);
        new_row->dirty = true;
    }

    /* Free old grid */
    for (int r = 0; r < grid->num_rows; r++)
        grid_row_free(old_grid[r]);
    free(grid->rows);

    grid->rows = new_grid;
    grid->num_rows = new_rows;
    grid->num_cols = new_cols;

    grid->view = grid->offset = new_offset;

    /* Keep cursor at current position, but clamp to new dimensions */
    struct coord cursor = grid->cursor.point;
    if (cursor.row == old_screen_rows - 1) {
        /* 'less' breaks if the cursor isn't at the bottom */
        cursor.row = new_screen_rows - 1;
    }
    cursor.row = min(cursor.row, new_screen_rows - 1);
    cursor.col = min(cursor.col, new_cols - 1);
    grid->cursor.point = cursor;

    struct coord saved_cursor = grid->saved_cursor.point;
    if (saved_cursor.row == old_screen_rows - 1)
        saved_cursor.row = new_screen_rows - 1;
    saved_cursor.row = min(saved_cursor.row, new_screen_rows - 1);
    saved_cursor.col = min(saved_cursor.col, new_cols - 1);
    grid->saved_cursor.point = saved_cursor;

    grid->cur_row = new_grid[(grid->offset + cursor.row) & (new_rows - 1)];
    grid->cursor.lcf = false;
    grid->saved_cursor.lcf = false;

    /* Free sixels we failed to "map" to the new grid */
    tll_foreach(untranslated_sixels, it)
        sixel_destroy(&it->item);
    tll_free(untranslated_sixels);

#if defined(_DEBUG)
    for (int r = 0; r < new_screen_rows; r++)
        grid_row_in_view(grid, r);
#endif
}

static void
reflow_uri_ranges(const struct row *old_row, struct row *new_row,
                  int old_col_idx, int new_col_idx)
{
    if (old_row->extra == NULL)
        return;

    /*
     * Check for URI range start/end points on the “old” row, and
     * open/close a corresponding URI range on the “new” row.
     */

    tll_foreach(old_row->extra->uri_ranges, it) {
        if (it->item.start == old_col_idx) {
            struct row_uri_range new_range = {
                .start = new_col_idx,
                .end = -1,
                .id = it->item.id,
                .uri = xstrdup(it->item.uri),
            };
            grid_row_add_uri_range(new_row, new_range);
        }

        if (it->item.end == old_col_idx) {
            xassert(new_row->extra != NULL);

            bool found_it = false;
            tll_foreach(new_row->extra->uri_ranges, it2) {
                if (it2->item.id != it->item.id)
                    continue;
                if (it2->item.end >= 0)
                    continue;

                it2->item.end = new_col_idx;
                found_it = true;
                break;
            }
            xassert(found_it);
        }
    }
}

static struct row *
_line_wrap(struct grid *old_grid, struct row **new_grid, struct row *row,
           int *row_idx, int *col_idx, int row_count, int col_count)
{
    *col_idx = 0;
    *row_idx = (*row_idx + 1) & (row_count - 1);

    struct row *new_row = new_grid[*row_idx];

    if (new_row == NULL) {
        /* Scrollback not yet full, allocate a completely new row */
        new_row = grid_row_alloc(col_count, false);
        new_grid[*row_idx] = new_row;
    } else {
        /* Scrollback is full, need to re-use a row */
        grid_row_reset_extra(new_row);
        new_row->linebreak = false;

        tll_foreach(old_grid->sixel_images, it) {
            if (it->item.pos.row == *row_idx) {
                sixel_destroy(&it->item);
                tll_remove(old_grid->sixel_images, it);
            }
        }
    }

    if (row->extra == NULL)
        return new_row;

    /*
     * URI ranges are per row. Thus, we need to ‘close’ the still-open
     * ranges on the previous row, and re-open them on the
     * next/current row.
     */
    tll_foreach(row->extra->uri_ranges, it) {
        if (it->item.end >= 0)
            continue;

        /* Terminate URI range on the previous row */
        it->item.end = col_count - 1;

        /* Open a new range on the new/current row */
        struct row_uri_range new_range = {
            .start = 0,
            .end = -1,
            .id = it->item.id,
            .uri = xstrdup(it->item.uri),
        };
        grid_row_add_uri_range(new_row, new_range);
    }

    return new_row;
}

static struct {
    int scrollback_start;
    int rows;
} tp_cmp_ctx;

static int
tp_cmp(const void *_a, const void *_b)
{
    const struct coord *a = *(const struct coord **)_a;
    const struct coord *b = *(const struct coord **)_b;

    int scrollback_start = tp_cmp_ctx.scrollback_start;
    int num_rows = tp_cmp_ctx.rows;

    int a_row = (a->row - scrollback_start + num_rows) & (num_rows - 1);
    int b_row = (b->row - scrollback_start + num_rows) & (num_rows - 1);

    xassert(a_row >= 0);
    xassert(a_row < num_rows || num_rows == 0);
    xassert(b_row >= 0);
    xassert(b_row < num_rows || num_rows == 0);

    if (a_row < b_row)
        return -1;
    if (a_row > b_row)
        return 1;

    xassert(a_row == b_row);

    if (a->col < b->col)
        return -1;
    if (a->col > b->col)
        return 1;

    xassert(a->col == b->col);
    return 0;
}

void
grid_resize_and_reflow(
    struct grid *grid, int new_rows, int new_cols,
    int old_screen_rows, int new_screen_rows,
    size_t tracking_points_count,
    struct coord *const _tracking_points[static tracking_points_count],
    size_t compose_count, const struct
    composed composed[static compose_count])
{
#if defined(TIME_REFLOW) && TIME_REFLOW
    struct timeval start;
    gettimeofday(&start, NULL);
#endif

    struct row *const *old_grid = grid->rows;
    const int old_rows = grid->num_rows;
    const int old_cols = grid->num_cols;

    /* Is viewpoint tracking current grid offset? */
    const bool view_follows = grid->view == grid->offset;

    int new_col_idx = 0;
    int new_row_idx = 0;

    struct row **new_grid = xcalloc(new_rows, sizeof(new_grid[0]));
    struct row *new_row = new_grid[new_row_idx];

    xassert(new_row == NULL);
    new_row = grid_row_alloc(new_cols, false);
    new_grid[new_row_idx] = new_row;

    /* Start at the beginning of the old grid's scrollback. That is,
     * at the output that is *oldest* */
    int offset = grid->offset + old_screen_rows;

    tll(struct sixel) untranslated_sixels = tll_init();
    tll_foreach(grid->sixel_images, it)
        tll_push_back(untranslated_sixels, it->item);
    tll_free(grid->sixel_images);

    /* Turn cursor coordinates into grid absolute coordinates */
    struct coord cursor = grid->cursor.point;
    cursor.row += grid->offset;
    cursor.row &= old_rows - 1;

    struct coord saved_cursor = grid->saved_cursor.point;
    saved_cursor.row += grid->offset;
    saved_cursor.row &= old_rows - 1;

    size_t tp_count =
        tracking_points_count +
        1 +                       /* cursor */
        1 +                       /* saved cursor */
        !view_follows +           /* viewport */
        1;                        /* terminator */

    struct coord *tracking_points[tp_count];
    memcpy(tracking_points, _tracking_points, tracking_points_count * sizeof(_tracking_points[0]));
    tracking_points[tracking_points_count] = &cursor;
    tracking_points[tracking_points_count + 1] = &saved_cursor;

    struct coord viewport = {0, grid->view};
    if (!view_follows)
        tracking_points[tracking_points_count + 2] = &viewport;

    /* Not thread safe! */
    tp_cmp_ctx.scrollback_start = offset;
    tp_cmp_ctx.rows = old_rows;
    qsort(
        tracking_points, tp_count - 1, sizeof(tracking_points[0]), &tp_cmp);

    /* NULL terminate */
    struct coord terminator = {-1, -1};
    tracking_points[tp_count - 1] = &terminator;
    struct coord **next_tp = &tracking_points[0];

    LOG_DBG("scrollback-start=%d", offset);
    for (size_t i = 0; i < tp_count - 1; i++) {
        LOG_DBG("TP #%zu: row=%d, col=%d",
                i, tracking_points[i]->row, tracking_points[i]->col);
    }

    /*
     * Walk the old grid
     */
    for (int r = 0; r < old_rows; r++) {

        const size_t old_row_idx = (offset + r) & (old_rows - 1);

        /* Unallocated (empty) rows we can simply skip */
        const struct row *old_row = old_grid[old_row_idx];
        if (old_row == NULL)
            continue;

        /* Map sixels on current "old" row to current "new row" */
        tll_foreach(untranslated_sixels, it) {
            if (it->item.pos.row != old_row_idx)
                continue;

            struct sixel sixel = it->item;
            sixel.pos.row = new_row_idx;

            tll_push_back(grid->sixel_images, sixel);
            tll_remove(untranslated_sixels, it);
        }

#define line_wrap()                                                 \
        new_row = _line_wrap(                                       \
            grid, new_grid, new_row, &new_row_idx, &new_col_idx,    \
            new_rows, new_cols)

#define print_spacer(remaining)                                         \
        do {                                                            \
            new_row->cells[new_col_idx].wc = CELL_SPACER + (remaining); \
            new_row->cells[new_col_idx].attrs = old_cell->attrs;        \
        } while (0)

        /*
         * Keep track of empty cells. If the old line ends with a
         * string of empty cells, we don't need to, nor do we want to,
         * add those to the new line. However, if there are non-empty
         * cells *after* the string of empty cells, we need to emit
         * the empty cells too. And that may trigger linebreaks
         */
        int empty_count = 0;

        /* Walk current line of the old grid */
        for (int c = 0; c < old_cols; c++) {
            const struct cell *old_cell = &old_row->cells[c];
            wchar_t wc = old_cell->wc;

            /* Check if this cell is one of the tracked cells */
            bool is_tracking_point = false;

            struct coord *tp = *next_tp;
            if (unlikely(tp->row == old_row_idx && tp->col == c))
                is_tracking_point = true;

            /* If there’s an URI start/end point here, we need to make
             * sure we handle it */
            bool on_uri = false;
            if (old_row->extra != NULL) {
                tll_foreach(old_row->extra->uri_ranges, it) {
                    if (unlikely(it->item.start == c || it->item.end == c)) {
                        on_uri = true;
                        break;
                    }
                }
            }

            if (wc == 0 && likely(!(is_tracking_point | on_uri))) {
                empty_count++;
                continue;
            }

            /* Allow left-adjusted and right-adjusted text, with empty
             * cells in between, to be "pushed together" */
            int old_cols_left = old_cols - c;
            int cols_needed = empty_count + old_cols_left;
            int new_cols_left = new_cols - new_col_idx;
            if (new_cols_left < cols_needed && new_cols_left >= old_cols_left)
                empty_count = max(0, empty_count - (cols_needed - new_cols_left));

            for (int i = 0; i < empty_count; i++) {
                if (new_col_idx + 1 > new_cols)
                    line_wrap();

                size_t idx = c - empty_count + i;

                new_row->cells[new_col_idx].wc = 0;
                new_row->cells[new_col_idx].attrs = old_row->cells[idx].attrs;
                new_col_idx++;
            }

            empty_count = 0;

            if (wc == CELL_SPACER)
                continue;

            if (unlikely(wc < CELL_SPACER &&
                         c + 1 < old_cols &&
                         old_row->cells[c + 1].wc > CELL_SPACER))
            {
                int width = old_row->cells[c + 1].wc - CELL_SPACER + 1;
                assert(wcwidth(wc) == width);

                /* Out of columns on current row in new grid? */
                if (new_col_idx + width > new_cols) {
                    /* Pad to end-of-line with spacers, then line-wrap */
                    for (;new_col_idx < new_cols; new_col_idx++)
                        print_spacer(0);
                    line_wrap();
                }
            }

            if (new_col_idx + 1 > new_cols)
                line_wrap();

            xassert(new_row != NULL);
            xassert(new_col_idx >= 0);
            xassert(new_col_idx < new_cols);

            new_row->cells[new_col_idx] = *old_cell;

            /* Translate tracking point(s) */
            if (unlikely(is_tracking_point)) {
                do {
                    xassert(tp != NULL);
                    xassert(tp->row == old_row_idx);
                    xassert(tp->col == c);

                    tp->row = new_row_idx;
                    tp->col = new_col_idx;

                    next_tp++;
                    tp = *next_tp;
                } while (tp->row == old_row_idx && tp->col == c);
            }

            if (unlikely(on_uri))
                reflow_uri_ranges(old_row, new_row, c, new_col_idx);

            new_col_idx++;
        }

        if (old_row->linebreak) {
            /* Erase the remaining cells */
            memset(&new_row->cells[new_col_idx], 0,
                   (new_cols - new_col_idx) * sizeof(new_row->cells[0]));
            new_row->linebreak = true;
            line_wrap();
        }

        grid_row_free(old_grid[old_row_idx]);
        grid->rows[old_row_idx] = NULL;

#undef print_spacer
#undef line_wrap
    }

    /* Erase the remaining cells */
    memset(&new_row->cells[new_col_idx], 0,
           (new_cols - new_col_idx) * sizeof(new_row->cells[0]));

    xassert(old_rows == 0 || *next_tp == &terminator);

#if defined(_DEBUG)
    /* Verify all URI ranges have been “closed” */
    for (int r = 0; r < new_rows; r++) {
        const struct row *row = new_grid[r];

        if (row == NULL)
            continue;
        if (row->extra == NULL)
            continue;

        tll_foreach(row->extra->uri_ranges, it)
            xassert(it->item.end >= 0);
    }

    /* Verify all old rows have been free:d */
    for (int i = 0; i < old_rows; i++)
        xassert(grid->rows[i] == NULL);
#endif

    /* Set offset such that the last reflowed row is at the bottom */
    grid->offset = new_row_idx - new_screen_rows + 1;
    while (grid->offset < 0)
        grid->offset += new_rows;
    while (new_grid[grid->offset] == NULL)
        grid->offset = (grid->offset + 1) & (new_rows - 1);

    /* Ensure all visible rows have been allocated */
    for (int r = 0; r < new_screen_rows; r++) {
        int idx = (grid->offset + r) & (new_rows - 1);
        if (new_grid[idx] == NULL)
            new_grid[idx] = grid_row_alloc(new_cols, true);
    }

    grid->view = view_follows ? grid->offset : viewport.row;

    /* If enlarging the window, the old viewport may be too far down,
     * with unallocated rows. Make sure this cannot happen */
    while (true) {
        int idx = (grid->view + new_screen_rows - 1) & (new_rows - 1);
        if (new_grid[idx] != NULL)
            break;
        grid->view--;
        if (grid->view < 0)
            grid->view += new_rows;
    }
    for (size_t r = 0; r < new_screen_rows; r++) {
        int UNUSED idx = (grid->view + r) & (new_rows - 1);
        xassert(new_grid[idx] != NULL);
    }

    /* Free old grid (rows already free:d) */
    free(grid->rows);

    grid->rows = new_grid;
    grid->num_rows = new_rows;
    grid->num_cols = new_cols;

    /* Convert absolute coordinates to screen relative */
    cursor.row -= grid->offset;
    while (cursor.row < 0)
        cursor.row += grid->num_rows;
    cursor.row = min(cursor.row, new_screen_rows - 1);
    cursor.col = min(cursor.col, new_cols - 1);

    saved_cursor.row -= grid->offset;
    while (saved_cursor.row < 0)
        saved_cursor.row += grid->num_rows;
    saved_cursor.row = min(saved_cursor.row, new_screen_rows - 1);
    saved_cursor.col = min(saved_cursor.col, new_cols - 1);

    grid->cur_row = new_grid[(grid->offset + cursor.row) & (new_rows - 1)];
    grid->cursor.point = cursor;
    grid->saved_cursor.point = saved_cursor;

    grid->cursor.lcf = false;
    grid->saved_cursor.lcf = false;

    /* Free sixels we failed to "map" to the new grid */
    tll_foreach(untranslated_sixels, it)
        sixel_destroy(&it->item);
    tll_free(untranslated_sixels);

#if defined(TIME_REFLOW) && TIME_REFLOW
    struct timeval stop;
    gettimeofday(&stop, NULL);

    struct timeval diff;
    timersub(&stop, &start, &diff);
    LOG_INFO("reflowed %d -> %d rows in %llds %lldµs",
             old_rows, new_rows,
             (long long)diff.tv_sec,
             (long long)diff.tv_usec);
#endif
}

static void
ensure_row_has_extra_data(struct row *row)
{
    if (row->extra == NULL)
        row->extra = xcalloc(1, sizeof(*row->extra));
}

void
grid_row_add_uri_range(struct row *row, struct row_uri_range range)
{
    ensure_row_has_extra_data(row);
    tll_push_back(row->extra->uri_ranges, range);
}
