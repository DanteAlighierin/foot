#pragma once

#include <stddef.h>
#include "terminal.h"

void grid_swap_row(struct grid *grid, int row_a, int row_b, bool initialize);
struct row *grid_row_alloc(int cols, bool initialize);
void grid_row_free(struct row *row);

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
    assert(grid->offset >= 0);

    int real_row = grid_row_absolute(grid, row_no);
    struct row *row = grid->rows[real_row];

    if (row == NULL && alloc_if_null) {
        row = grid_row_alloc(grid->num_cols, false);
        grid->rows[real_row] = row;
    }

    assert(row != NULL);
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
    assert(grid->view >= 0);

    int real_row = grid_row_absolute_in_view(grid, row_no);
    struct row *row = grid->rows[real_row];

    assert(row != NULL);
    return row;
}
