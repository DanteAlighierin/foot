#pragma once

#include <stddef.h>
#include "terminal.h"

static inline struct row *
grid_row(struct grid *grid, int row)
{
    assert(grid->offset >= 0);
    return grid->rows[(grid->offset + row + grid->num_rows) % grid->num_rows];
}

static inline struct row *
grid_row_in_view(struct grid *grid, int row)
{
    assert(grid->view >= 0);
    return grid->rows[(grid->view + row + grid->num_rows) % grid->num_rows];
}

void grid_swap_row(struct grid *grid, int row_a, int row_b);
