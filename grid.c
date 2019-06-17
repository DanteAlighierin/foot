#include "grid.h"

#include <string.h>
#include <assert.h>

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

void
grid_erase(struct grid *grid, int start, int end)
{
    for (int i = start; i < end; i++) {
        struct cell *cell = &grid->cells[i];
        memset(cell, 0, sizeof(*cell));

        cell->attrs.foreground = grid->foreground;
        cell->attrs.background = grid->background;

        cell->dirty = true;
        grid->dirty = true;
    }
}

int
grid_cursor_linear(const struct grid *grid, int row, int col)
{
    return row * grid->cols + col;
}

void
grid_cursor_to(struct grid *grid, int row, int col)
{
    assert(row >= 0);
    assert(row < grid->rows);
    assert(col >= 0);
    assert(col < grid->cols);

    int new_linear = row * grid->cols + col;
    assert(new_linear >= 0);
    assert(new_linear < grid->rows * grid->cols);

    grid->cells[grid->linear_cursor].dirty = true;
    grid->cells[new_linear].dirty = true;
    grid->dirty = true;
    grid->print_needs_wrap = false;

    grid->linear_cursor = new_linear;
    grid->cursor.col = col;
    grid->cursor.row = row;
}

void
grid_cursor_left(struct grid *grid, int count)
{
    int move_amount = min(grid->cursor.col, count);
    int new_linear = grid->linear_cursor - move_amount;
    int new_col = grid->cursor.col - move_amount;

    assert(new_linear >= 0);
    assert(new_linear < grid->rows * grid->cols);
    assert(new_col >= 0);
    assert(new_col < grid->cols);

    grid->cells[grid->linear_cursor].dirty = true;
    grid->cells[new_linear].dirty = true;

    grid->linear_cursor = new_linear;
    grid->cursor.col = new_col;

    grid->dirty = true;
    grid->print_needs_wrap = false;
}

void
grid_cursor_right(struct grid *grid, int count)
{
    int move_amount = min(grid->cols - grid->cursor.col - 1, count);
    int new_linear = grid->linear_cursor + move_amount;
    int new_col = grid->cursor.col + move_amount;

    assert(new_linear >= 0);
    assert(new_linear < grid->rows * grid->cols);
    assert(new_col >= 0);
    assert(new_col < grid->cols);

    grid->cells[grid->linear_cursor].dirty = true;
    grid->cells[new_linear].dirty = true;

    grid->linear_cursor = new_linear;
    grid->cursor.col = new_col;

    grid->dirty = true;
    grid->print_needs_wrap = false;
}
