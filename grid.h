#pragma once

#include <stddef.h>
#include "terminal.h"

void grid_swap_row(struct grid *grid, int row_a, int row_b);
struct row *grid_row_alloc(int cols);
void grid_row_free(struct row *row);

static inline struct row *
grid_row(struct grid *grid, int row_no)
{
    assert(grid->offset >= 0);

    int real_row = (grid->offset + row_no + grid->num_rows) % grid->num_rows;
    struct row *row = grid->rows[real_row];

    if (row == NULL) {
        row = grid_row_alloc(grid->num_cols);
        grid->rows[real_row] = row;
    }

    __builtin_prefetch(row->cells, 1, 3);
    return row;
}

static inline struct row *
grid_row_in_view(struct grid *grid, int row_no)
{
    assert(grid->view >= 0);

    int real_row = (grid->view + row_no + grid->num_rows) % grid->num_rows;
    struct row *row = grid->rows[real_row];

    assert(row != NULL);
    return row;
}
