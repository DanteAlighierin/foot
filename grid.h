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

static inline void
grid_swap_row(struct grid *grid, int row_a, int row_b)
{
    assert(grid->offset >= 0);
    assert(row_a != row_b);
    assert(row_a >= 0);
    assert(row_b >= 0);

    int real_a = (grid->offset + row_a + grid->num_rows) % grid->num_rows;
    int real_b = (grid->offset + row_b + grid->num_rows) % grid->num_rows;

    struct row *tmp = grid->rows[real_a];
    grid->rows[real_a] = grid->rows[real_b];
    grid->rows[real_b] = tmp;

    grid->rows[real_a]->dirty = true;
    grid->rows[real_b]->dirty = true;
}

#if 0
struct cell *grid_get_range(struct grid *grid, int start, int *length);
void grid_memclear(struct grid *grid, int start, int length);
void grid_memmove(struct grid *grid, int dst, int src, int length);
#endif
