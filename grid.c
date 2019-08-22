#include "grid.h"

//#include <string.h>
#include <assert.h>

#define LOG_MODULE "grid"
#define LOG_ENABLE_DBG 0
#include "log.h"

void
grid_swap_row(struct grid *grid, int row_a, int row_b, bool initialize)
{
    assert(grid->offset >= 0);
    assert(row_a != row_b);

    int real_a = (grid->offset + row_a) & (grid->num_rows - 1);
    int real_b = (grid->offset + row_b) & (grid->num_rows - 1);

    struct row *a = grid->rows[real_a];
    struct row *b = grid->rows[real_b];
#if 0
    if (a == NULL)
        a = grid_row_alloc(grid->num_cols, initialize);
    if (b == NULL)
        b = grid_row_alloc(grid->num_cols, initialize);
#endif
    grid->rows[real_a] = b;
    grid->rows[real_b] = a;
}

struct row *
grid_row_alloc(int cols, bool initialize)
{
    struct row *row = malloc(sizeof(*row));
    row->dirty = false;

    if (initialize) {
        row->cells = calloc(cols, sizeof(row->cells[0]));
        for (size_t c = 0; c < cols; c++)
            row->cells[c].attrs.clean = 1;
    } else
        row->cells = malloc(cols * sizeof(row->cells[0]));

    return row;
}

void
grid_row_free(struct row *row)
{
    if (row == NULL)
        return;

    free(row->cells);
    free(row);
}
