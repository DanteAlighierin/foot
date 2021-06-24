#pragma once

#include <stddef.h>
#include "debug.h"
#include "terminal.h"

struct grid *grid_snapshot(const struct grid *grid);
void grid_free(struct grid *grid);

void grid_swap_row(struct grid *grid, int row_a, int row_b);
struct row *grid_row_alloc(int cols, bool initialize);
void grid_row_free(struct row *row);

void grid_resize_without_reflow(
    struct grid *grid, int new_rows, int new_cols,
    int old_screen_rows, int new_screen_rows);

void grid_resize_and_reflow(
    struct grid *grid, int new_rows, int new_cols,
    int old_screen_rows, int new_screen_rows,
    size_t tracking_points_count,
    struct coord *const _tracking_points[static tracking_points_count]);

static inline int
grid_row_absolute(const struct grid *grid, int row_no)
{
    return (grid->offset + row_no) & (grid->num_rows - 1);
}

static inline int
grid_row_absolute_in_view(const struct grid *grid, int row_no)
{
    return (grid->view + row_no) & (grid->num_rows - 1);
}

static inline struct row *
_grid_row_maybe_alloc(struct grid *grid, int row_no, bool alloc_if_null)
{
    xassert(grid->offset >= 0);

    int real_row = grid_row_absolute(grid, row_no);
    struct row *row = grid->rows[real_row];

    if (row == NULL && alloc_if_null) {
        row = grid_row_alloc(grid->num_cols, false);
        grid->rows[real_row] = row;
    }

    xassert(row != NULL);
    return row;
}

static inline struct row *
grid_row(struct grid *grid, int row_no)
{
    return _grid_row_maybe_alloc(grid, row_no, false);
}

static inline struct row *
grid_row_and_alloc(struct grid *grid, int row_no)
{
    return _grid_row_maybe_alloc(grid, row_no, true);
}

static inline struct row *
grid_row_in_view(struct grid *grid, int row_no)
{
    xassert(grid->view >= 0);

    int real_row = grid_row_absolute_in_view(grid, row_no);
    struct row *row = grid->rows[real_row];

    xassert(row != NULL);
    return row;
}

void grid_row_add_uri_range(struct row *row, struct row_uri_range range);

static inline void
grid_row_uri_range_destroy(struct row_uri_range *range)
{
    free(range->uri);
}

static inline void
grid_row_reset_extra(struct row *row)
{
    if (likely(row->extra == NULL))
        return;

    tll_foreach(row->extra->uri_ranges, it) {
        grid_row_uri_range_destroy(&it->item);
        tll_remove(row->extra->uri_ranges, it);
    }

    free(row->extra);
    row->extra = NULL;
}
