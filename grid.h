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

/* Convert row numbers between scrollback-relative and absolute coordinates */
int grid_row_abs_to_sb(const struct grid *grid, int screen_rows, int abs_row);
int grid_row_sb_to_abs(const struct grid *grid, int screen_rows, int sb_rel_row);

int grid_sb_start_ignore_uninitialized(const struct grid *grid, int screen_rows);
int grid_row_abs_to_sb_precalc_sb_start(
    const struct grid *grid, int sb_start, int abs_row);
int grid_row_sb_to_abs_precalc_sb_start(
    const struct grid *grid, int sb_start, int sb_rel_row);

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

void grid_row_uri_range_put(
    struct row *row, int col, const char *uri, uint64_t id);
void grid_row_uri_range_add(struct row *row, struct row_uri_range range);
void grid_row_uri_range_erase(struct row *row, int start, int end);

static inline void
grid_row_uri_range_destroy(struct row_uri_range *range)
{
    free(range->uri);
}

static inline void
grid_row_reset_extra(struct row *row)
{
    struct row_data *extra = row->extra;

    if (likely(extra == NULL))
        return;

    for (size_t i = 0; i < extra->uri_ranges.count; i++)
        grid_row_uri_range_destroy(&extra->uri_ranges.v[i]);
    free(extra->uri_ranges.v);

    free(extra);
    row->extra = NULL;
}
