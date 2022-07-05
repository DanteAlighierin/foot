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

#define TIME_REFLOW 0

/*
 * “sb” (scrollback relative) coordinates
 *
 * The scrollback relative row number 0 is the *first*, and *oldest*
 * row in the scrollback history (and thus the *first* row to be
 * scrolled out). Thus, a higher number means further *down* in the
 * scrollback, with the *highest* number being at the bottom of the
 * screen, where new input appears.
 */

int
grid_row_abs_to_sb(const struct grid *grid, int screen_rows, int abs_row)
{
    const int scrollback_start = grid->offset + screen_rows;
    int rebased_row = abs_row - scrollback_start + grid->num_rows;

    rebased_row &= grid->num_rows - 1;
    return rebased_row;
}

int grid_row_sb_to_abs(const struct grid *grid, int screen_rows, int sb_rel_row)
{
    const int scrollback_start = grid->offset + screen_rows;
    int abs_row = sb_rel_row + scrollback_start;

    abs_row &= grid->num_rows - 1;
    return abs_row;
}

int
grid_sb_start_ignore_uninitialized(const struct grid *grid, int screen_rows)
{
    int scrollback_start = grid->offset + screen_rows;
    scrollback_start &= grid->num_rows - 1;

    while (grid->rows[scrollback_start] == NULL) {
        scrollback_start++;
        scrollback_start &= grid->num_rows - 1;
    }

    return scrollback_start;
}

int
grid_row_abs_to_sb_precalc_sb_start(const struct grid *grid, int sb_start,
                                    int abs_row)
{
    int rebased_row = abs_row - sb_start + grid->num_rows;
    rebased_row &= grid->num_rows - 1;
    return rebased_row;
}

int
grid_row_sb_to_abs_precalc_sb_start(const struct grid *grid, int sb_start,
                                    int sb_rel_row)
{
    int abs_row = sb_rel_row + sb_start;
    abs_row &= grid->num_rows - 1;
    return abs_row;
}

static void
ensure_row_has_extra_data(struct row *row)
{
    if (row->extra == NULL)
        row->extra = xcalloc(1, sizeof(*row->extra));
}

static void
verify_no_overlapping_uris(const struct row_data *extra)
{
#if defined(_DEBUG)
    for (size_t i = 0; i < extra->uri_ranges.count; i++) {
        const struct row_uri_range *r1 = &extra->uri_ranges.v[i];

        for (size_t j = i + 1; j < extra->uri_ranges.count; j++) {
            const struct row_uri_range *r2 = &extra->uri_ranges.v[j];
            xassert(r1 != r2);

            if ((r1->start <= r2->start && r1->end >= r2->start) ||
                (r1->start <= r2->end && r1->end >= r2->end))
            {
                BUG("OSC-8 URI overlap: %s: %d-%d: %s: %d-%d",
                    r1->uri, r1->start, r1->end,
                    r2->uri, r2->start, r2->end);
            }
        }
    }
#endif
}

static void
verify_uris_are_sorted(const struct row_data *extra)
{
#if defined(_DEBUG)
    const struct row_uri_range *last = NULL;

    for (size_t i = 0; i < extra->uri_ranges.count; i++) {
        const struct row_uri_range *r = &extra->uri_ranges.v[i];

        if (last != NULL) {
            if (last->start >= r->start || last->end >= r->end) {
                BUG("OSC-8 URI not sorted correctly: "
                    "%s: %d-%d came before %s: %d-%d",
                    last->uri, last->start, last->end,
                    r->uri, r->start, r->end);
            }
        }

        last = r;
    }
#endif
}

static void
uri_range_ensure_size(struct row_data *extra, uint32_t count_to_add)
{
    if (extra->uri_ranges.count + count_to_add > extra->uri_ranges.size) {
        extra->uri_ranges.size = extra->uri_ranges.count + count_to_add;
        extra->uri_ranges.v = xrealloc(
            extra->uri_ranges.v,
            extra->uri_ranges.size * sizeof(extra->uri_ranges.v[0]));
    }

    xassert(extra->uri_ranges.count + count_to_add <= extra->uri_ranges.size);
}

/*
 * Be careful! This function may xrealloc() the URI range vector, thus
 * invalidating pointers into it.
 */
static void
uri_range_insert(struct row_data *extra, size_t idx, int start, int end,
                 uint64_t id, const char *uri)
{
    uri_range_ensure_size(extra, 1);

    xassert(idx <= extra->uri_ranges.count);

    const size_t move_count = extra->uri_ranges.count - idx;
    memmove(&extra->uri_ranges.v[idx + 1],
            &extra->uri_ranges.v[idx],
            move_count * sizeof(extra->uri_ranges.v[0]));

    extra->uri_ranges.count++;
    extra->uri_ranges.v[idx] = (struct row_uri_range){
        .start = start,
        .end = end,
        .id = id,
        .uri = xstrdup(uri),
    };
}

static void
uri_range_append_no_strdup(struct row_data *extra, int start, int end,
                           uint64_t id, char *uri)
{
    uri_range_ensure_size(extra, 1);
    extra->uri_ranges.v[extra->uri_ranges.count++] = (struct row_uri_range){
        .start = start,
        .end = end,
        .id = id,
        .uri = uri,
    };
}

static void
uri_range_append(struct row_data *extra, int start, int end, uint64_t id,
                 const char *uri)
{
    uri_range_append_no_strdup(extra, start, end, id, xstrdup(uri));
}

static void
uri_range_delete(struct row_data *extra, size_t idx)
{
    xassert(idx < extra->uri_ranges.count);
    grid_row_uri_range_destroy(&extra->uri_ranges.v[idx]);

    const size_t move_count = extra->uri_ranges.count - idx - 1;
    memmove(&extra->uri_ranges.v[idx],
            &extra->uri_ranges.v[idx + 1],
            move_count * sizeof(extra->uri_ranges.v[0]));
    extra->uri_ranges.count--;
}

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
        clone_row->prompt_marker = row->prompt_marker;

        for (int c = 0; c < grid->num_cols; c++)
            clone_row->cells[c] = row->cells[c];

        const struct row_data *extra = row->extra;

        if (extra != NULL) {
            struct row_data *clone_extra = xcalloc(1, sizeof(*clone_extra));
            clone_row->extra = clone_extra;

            uri_range_ensure_size(clone_extra, extra->uri_ranges.count);

            for (size_t i = 0; i < extra->uri_ranges.count; i++) {
                const struct row_uri_range *range = &extra->uri_ranges.v[i];
                uri_range_append(
                    clone_extra,
                    range->start, range->end, range->id, range->uri);
            }
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
    row->prompt_marker = false;

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
        new_row->prompt_marker = old_row->prompt_marker;

        if (new_cols > old_cols) {
            /* Clear "new" columns */
            memset(&new_row->cells[old_cols], 0,
                   sizeof(struct cell) * (new_cols - old_cols));
            new_row->dirty = true;
        } else if (old_cols > new_cols) {
            /* Make sure we don't cut a multi-column character in two */
            for (int i = new_cols; i > 0 && old_row->cells[i].wc > CELL_SPACER; i--)
                new_row->cells[i - 1].wc = 0;
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
        const struct row_data *old_extra = old_row->extra;
        if (old_extra == NULL)
            continue;

        ensure_row_has_extra_data(new_row);
        struct row_data *new_extra = new_row->extra;

        uri_range_ensure_size(new_extra, old_extra->uri_ranges.count);

        for (size_t i = 0; i < old_extra->uri_ranges.count; i++) {
            const struct row_uri_range *range = &old_extra->uri_ranges.v[i];

            if (range->start >= new_cols) {
                /* The whole range is truncated */
                continue;
            }

            const int start = range->start;
            const int end = min(range->end, new_cols - 1);
            uri_range_append(new_extra, start, end, range->id, range->uri);
        }
    }

    /* Clear "new" lines */
    for (int r = min(old_screen_rows, new_screen_rows); r < new_screen_rows; r++) {
        struct row *new_row = grid_row_alloc(new_cols, false);
        new_grid[(new_offset + r) & (new_rows - 1)] = new_row;

        memset(new_row->cells, 0, sizeof(struct cell) * new_cols);
        new_row->dirty = true;
    }

#if defined(_DEBUG)
    for (size_t r = 0; r < new_rows; r++) {
        const struct row *row = new_grid[r];

        if (row == NULL)
            continue;
        if (row->extra == NULL)
            continue;

        verify_no_overlapping_uris(row->extra);
        verify_uris_are_sorted(row->extra);
    }
#endif

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
reflow_uri_range_start(struct row_uri_range *range, struct row *new_row,
                       int new_col_idx)
{
    ensure_row_has_extra_data(new_row);
    uri_range_append_no_strdup
        (new_row->extra, new_col_idx, -1, range->id, range->uri);
    range->uri = NULL;
}

static void
reflow_uri_range_end(struct row_uri_range *range, struct row *new_row,
                     int new_col_idx)
{
    struct row_data *extra = new_row->extra;
    xassert(extra->uri_ranges.count > 0);

    struct row_uri_range *new_range =
        &extra->uri_ranges.v[extra->uri_ranges.count - 1];

    xassert(new_range->id == range->id);
    xassert(new_range->end < 0);
    new_range->end = new_col_idx;
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
        new_row->prompt_marker = false;

        tll_foreach(old_grid->sixel_images, it) {
            if (it->item.pos.row == *row_idx) {
                sixel_destroy(&it->item);
                tll_remove(old_grid->sixel_images, it);
            }
        }

        /*
         * TODO: detect if the re-used row is covered by the
         * selection. Of so, cancel the selection. The problem: we
         * don’t know if we’ve translated the selection coordinates
         * yet.
         */
    }

    struct row_data *extra = row->extra;
    if (extra == NULL)
        return new_row;

    /*
     * URI ranges are per row. Thus, we need to ‘close’ the still-open
     * ranges on the previous row, and re-open them on the
     * next/current row.
     */
    if (extra->uri_ranges.count > 0) {
        struct row_uri_range *range =
            &extra->uri_ranges.v[extra->uri_ranges.count - 1];

        if (range->end < 0) {

            /* Terminate URI range on the previous row */
            range->end = col_count - 1;

            /* Open a new range on the new/current row */
            ensure_row_has_extra_data(new_row);
            uri_range_append(new_row->extra, 0, -1, range->id, range->uri);
        }
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
    struct coord *const _tracking_points[static tracking_points_count])
{
#if defined(TIME_REFLOW) && TIME_REFLOW
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
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

        /* Find last non-empty cell */
        int col_count = 0;
        for (int c = old_cols - 1; c >= 0; c--) {
            const struct cell *cell = &old_row->cells[c];
            if (!(cell->wc == 0 || cell->wc == CELL_SPACER)) {
                col_count = c + 1;
                break;
            }
        }

        if (!old_row->linebreak && col_count > 0) {
            /* Don’t truncate logical lines */
            col_count = old_cols;
        }

        xassert(col_count >= 0 && col_count <= old_cols);

        /* Do we have a (at least one) tracking point on this row */
        struct coord *tp;
        if (unlikely((*next_tp)->row == old_row_idx)) {
            tp = *next_tp;

            /* Find the *last* tracking point on this row */
            struct coord *last_on_row = tp;
            for (struct coord **iter = next_tp; (*iter)->row == old_row_idx; iter++)
                last_on_row = *iter;

            /* And make sure its end point is included in the col range */
            xassert(last_on_row->row == old_row_idx);
            col_count = max(col_count, last_on_row->col + 1);
        } else
            tp = NULL;

        /* Does this row have any URIs? */
        struct row_uri_range *range, *range_terminator;
        struct row_data *extra = old_row->extra;

        if (extra != NULL && extra->uri_ranges.count > 0) {
            range = &extra->uri_ranges.v[0];
            range_terminator = &extra->uri_ranges.v[extra->uri_ranges.count];

            /* Make sure the *last* URI range's end point is included
             * in the copy */
            const struct row_uri_range *last_on_row =
                &extra->uri_ranges.v[extra->uri_ranges.count - 1];
            col_count = max(col_count, last_on_row->end + 1);
        } else
            range = range_terminator = NULL;

        for (int start = 0, left = col_count; left > 0;) {
            int end;
            bool tp_break = false;
            bool uri_break = false;

            /*
             * Set end-coordinate for this chunk, by finding the next
             * point-of-interrest on this row.
             *
             * If there are no more tracking points, or URI ranges,
             * the end-coordinate will be at the end of the row,
             */
            if (range != range_terminator) {
                int uri_col = (range->start >= start ? range->start : range->end) + 1;

                if (tp != NULL) {
                    int tp_col = tp->col + 1;
                    end = min(tp_col, uri_col);

                    tp_break = end == tp_col;
                    uri_break = end == uri_col;
                    LOG_DBG("tp+uri break at %d (%d, %d)", end, tp_col, uri_col);
                } else {
                    end = uri_col;
                    uri_break = true;
                    LOG_DBG("uri break at %d", end);
                }
            } else if (tp != NULL) {
                end = tp->col + 1;
                tp_break = true;
                LOG_DBG("TP break at %d", end);
            } else
                end = col_count;

            int cols = end - start;
            xassert(cols > 0);
            xassert(start + cols <= old_cols);

            /*
             * Copy the row chunk to the new grid. Note that there may
             * be fewer cells left on the new row than what we have in
             * the chunk. I.e. the chunk may have to be split up into
             * multiple memcpy:ies.
             */

            for (int count = cols, from = start; count > 0;) {
                xassert(new_col_idx <= new_cols);
                int new_row_cells_left = new_cols - new_col_idx;

                /* Row full, emit newline and get a new, fresh, row */
                if (new_row_cells_left <= 0) {
                    line_wrap();
                    new_row_cells_left = new_cols;
                }

                /* Number of cells we can copy */
                int amount = min(count, new_row_cells_left);
                xassert(amount > 0);

                /*
                 * If we’re going to reach the end of the new row, we
                 * need to make sure we don’t end in the middle of a
                 * multi-column character.
                 */
                int spacers = 0;
                if (new_col_idx + amount >= new_cols) {
                    /*
                     * While the cell *after* the last cell is a CELL_SPACER
                     *
                     * This means we have a multi-column character
                     * that doesn’t fit on the current row. We need to
                     * push it to the next row, and insert CELL_SPACER
                     * cells as padding.
                     */
                    while (
                        unlikely(
                            amount > 1 &&
                            from + amount < old_cols &&
                            old_row->cells[from + amount].wc >= CELL_SPACER + 1))
                    {
                        amount--;
                        spacers++;
                    }

                    xassert(
                        amount == 1 ||
                        old_row->cells[from + amount - 1].wc <= CELL_SPACER + 1);
                }

                xassert(new_col_idx + amount <= new_cols);
                xassert(from + amount <= old_cols);

                if (from == 0)
                    new_row->prompt_marker = old_row->prompt_marker;

                memcpy(
                    &new_row->cells[new_col_idx], &old_row->cells[from],
                    amount * sizeof(struct cell));

                count -= amount;
                from += amount;
                new_col_idx += amount;

                xassert(new_col_idx <= new_cols);

                if (unlikely(spacers > 0)) {
                    xassert(new_col_idx + spacers == new_cols);

                    const struct cell *cell = &old_row->cells[from - 1];

                    for (int i = 0; i < spacers; i++, new_col_idx++) {
                        new_row->cells[new_col_idx].wc = CELL_SPACER;
                        new_row->cells[new_col_idx].attrs = cell->attrs;
                    }
                }
            }

            xassert(new_col_idx > 0);

            if (tp_break) {
                do {
                    xassert(tp != NULL);
                    xassert(tp->row == old_row_idx);
                    xassert(tp->col == end - 1);

                    tp->row = new_row_idx;
                    tp->col = new_col_idx - 1;

                    next_tp++;
                    tp = *next_tp;
                } while (tp->row == old_row_idx && tp->col == end - 1);

                if (tp->row != old_row_idx)
                    tp = NULL;

                LOG_DBG("next TP (tp=%p): %dx%d",
                        (void*)tp, (*next_tp)->row, (*next_tp)->col);
            }

            if (uri_break) {
                xassert(range != NULL);

                if (range->start == end - 1)
                    reflow_uri_range_start(range, new_row, new_col_idx - 1);

                if (range->end == end - 1) {
                    reflow_uri_range_end(range, new_row, new_col_idx - 1);
                    grid_row_uri_range_destroy(range);
                    range++;
                }
            }

            left -= cols;
            start += cols;
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

#undef line_wrap
    }

    /* Erase the remaining cells */
    memset(&new_row->cells[new_col_idx], 0,
           (new_cols - new_col_idx) * sizeof(new_row->cells[0]));

    for (struct coord **tp = next_tp; *tp != &terminator; tp++) {
        LOG_DBG("TP: row=%d, col=%d (old cols: %d, new cols: %d)",
                (*tp)->row, (*tp)->col, old_cols, new_cols);
    }
    xassert(old_rows == 0 || *next_tp == &terminator);

#if defined(_DEBUG)
    /* Verify all URI ranges have been “closed” */
    for (int r = 0; r < new_rows; r++) {
        const struct row *row = new_grid[r];

        if (row == NULL)
            continue;
        if (row->extra == NULL)
            continue;

        for (size_t i = 0; i < row->extra->uri_ranges.count; i++)
            xassert(row->extra->uri_ranges.v[i].end >= 0);

        verify_no_overlapping_uris(row->extra);
        verify_uris_are_sorted(row->extra);
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
    struct timespec stop;
    clock_gettime(CLOCK_MONOTONIC, &stop);

    struct timespec diff;
    timespec_sub(&stop, &start, &diff);
    LOG_INFO("reflowed %d -> %d rows in %lds %ldns",
             old_rows, new_rows,
             (long)diff.tv_sec,
             diff.tv_nsec);
#endif
}

void
grid_row_uri_range_put(struct row *row, int col, const char *uri, uint64_t id)
{
    ensure_row_has_extra_data(row);

    size_t insert_idx = 0;
    bool replace = false;
    bool run_merge_pass = false;

    struct row_data *extra = row->extra;
    for (ssize_t i = (ssize_t)extra->uri_ranges.count - 1; i >= 0; i--) {
        struct row_uri_range *r = &extra->uri_ranges.v[i];

        const bool matching_id = r->id == id;

        if (matching_id && r->end + 1 == col) {
            /* Extend existing URI’s tail */
            r->end++;
            goto out;
        }

        else if (r->end < col) {
            insert_idx = i + 1;
            break;
        }

        else if (r->start > col)
            continue;

        else {
            xassert(r->start <= col);
            xassert(r->end >= col);

            if (matching_id)
                goto out;

            if (r->start == r->end) {
                replace = true;
                run_merge_pass = true;
                insert_idx = i;
            } else if (r->start == col) {
                run_merge_pass = true;
                r->start++;
                insert_idx = i;
            } else if (r->end == col) {
                run_merge_pass = true;
                r->end--;
                insert_idx = i + 1;
            } else {
                xassert(r->start < col);
                xassert(r->end > col);

                uri_range_insert(extra, i + 1, col + 1, r->end, r->id, r->uri);

                /* The insertion may xrealloc() the vector, making our
                 * ‘old’ pointer invalid */
                r = &extra->uri_ranges.v[i];
                r->end = col - 1;
                xassert(r->start <= r->end);

                insert_idx = i + 1;
            }

            break;
        }
    }

    xassert(insert_idx <= extra->uri_ranges.count);

    if (replace) {
        grid_row_uri_range_destroy(&extra->uri_ranges.v[insert_idx]);
        extra->uri_ranges.v[insert_idx] = (struct row_uri_range){
            .start = col,
            .end = col,
            .id = id,
            .uri = xstrdup(uri),
        };
    } else
        uri_range_insert(extra, insert_idx, col, col, id, uri);

    if (run_merge_pass) {
        for (size_t i = 1; i < extra->uri_ranges.count; i++) {
            struct row_uri_range *r1 = &extra->uri_ranges.v[i - 1];
            struct row_uri_range *r2 = &extra->uri_ranges.v[i];

            if (r1->id == r2->id && r1->end + 1 == r2->start) {
                r1->end = r2->end;
                uri_range_delete(extra, i);
                i--;
            }
        }
    }

out:
    verify_no_overlapping_uris(extra);
    verify_uris_are_sorted(extra);
}

UNITTEST
{
    struct row_data row_data = {.uri_ranges = {0}};
    struct row row = {.extra = &row_data};

#define verify_range(idx, _start, _end, _id)                     \
    do {                                                         \
        xassert(idx < row_data.uri_ranges.count);                \
        xassert(row_data.uri_ranges.v[idx].start == _start);     \
        xassert(row_data.uri_ranges.v[idx].end == _end);         \
        xassert(row_data.uri_ranges.v[idx].id == _id);           \
    } while (0)

    grid_row_uri_range_put(&row, 0, "http://foo.bar", 123);
    grid_row_uri_range_put(&row, 1, "http://foo.bar", 123);
    grid_row_uri_range_put(&row, 2, "http://foo.bar", 123);
    grid_row_uri_range_put(&row, 3, "http://foo.bar", 123);
    xassert(row_data.uri_ranges.count == 1);
    verify_range(0, 0, 3, 123);

    /* No-op */
    grid_row_uri_range_put(&row, 0, "http://foo.bar", 123);
    xassert(row_data.uri_ranges.count == 1);
    verify_range(0, 0, 3, 123);

    /* Replace head */
    grid_row_uri_range_put(&row, 0, "http://head", 456);
    xassert(row_data.uri_ranges.count == 2);
    verify_range(0, 0, 0, 456);
    verify_range(1, 1, 3, 123);

    /* Replace tail */
    grid_row_uri_range_put(&row, 3, "http://tail", 789);
    xassert(row_data.uri_ranges.count == 3);
    verify_range(1, 1, 2, 123);
    verify_range(2, 3, 3, 789);

    /* Replace tail + extend head */
    grid_row_uri_range_put(&row, 2, "http://tail", 789);
    xassert(row_data.uri_ranges.count == 3);
    verify_range(1, 1, 1, 123);
    verify_range(2, 2, 3, 789);

    /* Replace + extend tail */
    grid_row_uri_range_put(&row, 1, "http://head", 456);
    xassert(row_data.uri_ranges.count == 2);
    verify_range(0, 0, 1, 456);
    verify_range(1, 2, 3, 789);

    /* Replace + extend, then splice */
    grid_row_uri_range_put(&row, 1, "http://tail", 789);
    grid_row_uri_range_put(&row, 2, "http://splice", 000);
    xassert(row_data.uri_ranges.count == 4);
    verify_range(0, 0, 0, 456);
    verify_range(1, 1, 1, 789);
    verify_range(2, 2, 2, 000);
    verify_range(3, 3, 3, 789);

    for (size_t i = 0; i < row_data.uri_ranges.count; i++)
        grid_row_uri_range_destroy(&row_data.uri_ranges.v[i]);
    free(row_data.uri_ranges.v);

#undef verify_range
}

void
grid_row_uri_range_erase(struct row *row, int start, int end)
{
    xassert(row->extra != NULL);
    xassert(start <= end);

    struct row_data *extra = row->extra;

    /* Split up, or remove, URI ranges affected by the erase */
    for (ssize_t i = (ssize_t)extra->uri_ranges.count - 1; i >= 0; i--) {
        struct row_uri_range *old = &extra->uri_ranges.v[i];

        if (old->end < start)
            return;

        if (old->start > end)
            continue;

        if (start <= old->start && end >= old->end) {
            /* Erase range covers URI completely - remove it */
            uri_range_delete(extra, i);
        }

        else if (start > old->start && end < old->end) {
            /* Erase range erases a part in the middle of the URI */
            uri_range_insert(
                extra, i + 1, end + 1, old->end, old->id, old->uri);

            /* The insertion may xrealloc() the vector, making our
             * ‘old’ pointer invalid */
            old = &extra->uri_ranges.v[i];
            old->end = start - 1;
            return;  /* There can be no more URIs affected by the erase range */
        }

        else if (start <= old->start && end >= old->start) {
            /* Erase range erases the head of the URI */
            xassert(start <= old->start);
            old->start = end + 1;
        }

        else if (start <= old->end && end >= old->end) {
            /* Erase range erases the tail of the URI */
            xassert(end >= old->end);
            old->end = start - 1;
            return;  /* There can be no more overlapping URIs */
        }
    }
}

UNITTEST
{
    struct row_data row_data = {.uri_ranges = {0}};
    struct row row = {.extra = &row_data};

    /* Try erasing a row without any URIs */
    grid_row_uri_range_erase(&row, 0, 200);
    xassert(row_data.uri_ranges.count == 0);

    uri_range_append(&row_data, 1, 10, 0, "dummy");
    uri_range_append(&row_data, 11, 20, 0, "dummy");
    xassert(row_data.uri_ranges.count == 2);
    xassert(row_data.uri_ranges.v[1].start == 11);
    xassert(row_data.uri_ranges.v[1].end == 20);
    verify_no_overlapping_uris(&row_data);
    verify_uris_are_sorted(&row_data);

    /* Erase both URis */
    grid_row_uri_range_erase(&row, 1, 20);
    xassert(row_data.uri_ranges.count == 0);
    verify_no_overlapping_uris(&row_data);
    verify_uris_are_sorted(&row_data);

    /* Two URIs, then erase second half of the first, first half of
       the second */
    uri_range_append(&row_data, 1, 10, 0, "dummy");
    uri_range_append(&row_data, 11, 20, 0, "dummy");
    grid_row_uri_range_erase(&row, 5, 15);
    xassert(row_data.uri_ranges.count == 2);
    xassert(row_data.uri_ranges.v[0].start == 1);
    xassert(row_data.uri_ranges.v[0].end == 4);
    xassert(row_data.uri_ranges.v[1].start == 16);
    xassert(row_data.uri_ranges.v[1].end == 20);
    verify_no_overlapping_uris(&row_data);
    verify_uris_are_sorted(&row_data);

    grid_row_uri_range_destroy(&row_data.uri_ranges.v[0]);
    grid_row_uri_range_destroy(&row_data.uri_ranges.v[1]);
    row_data.uri_ranges.count = 0;

    /* One URI, erase middle part of it */
    uri_range_append(&row_data, 1, 10, 0, "dummy");
    grid_row_uri_range_erase(&row, 5, 6);
    xassert(row_data.uri_ranges.count == 2);
    xassert(row_data.uri_ranges.v[0].start == 1);
    xassert(row_data.uri_ranges.v[0].end == 4);
    xassert(row_data.uri_ranges.v[1].start == 7);
    xassert(row_data.uri_ranges.v[1].end == 10);
    verify_no_overlapping_uris(&row_data);
    verify_uris_are_sorted(&row_data);

    grid_row_uri_range_destroy(&row_data.uri_ranges.v[0]);
    grid_row_uri_range_destroy(&row_data.uri_ranges.v[1]);
    row_data.uri_ranges.count = 0;

    /*
     * Regression test: erasing the middle part of an URI causes us to
     * insert a new URI (we split the partly erased URI into two).
     *
     * The insertion logic typically triggers an xrealloc(), which, in
     * some cases, *moves* the entire URI vector to a new base
     * address. grid_row_uri_range_erase() did not account for this,
     * and tried to update the ‘end’ member in the URI range we just
     * split. This causes foot to crash when the xrealloc() has moved
     * the URI range vector.
     *
     * (note: we’re only verifying we don’t crash here, hence the lack
     * of assertions).
     */
    free(row_data.uri_ranges.v);
    row_data.uri_ranges.v = NULL;
    row_data.uri_ranges.size = 0;
    uri_range_append(&row_data, 1, 10, 0, "dummy");
    xassert(row_data.uri_ranges.size == 1);

    grid_row_uri_range_erase(&row, 5, 7);
    xassert(row_data.uri_ranges.count == 2);

    for (size_t i = 0; i < row_data.uri_ranges.count; i++)
        grid_row_uri_range_destroy(&row_data.uri_ranges.v[i]);
    free(row_data.uri_ranges.v);
}
