#include "grid.h"

#include <string.h>
#include <assert.h>

#define LOG_MODULE "grid"
#define LOG_ENABLE_DBG 1
#include "log.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

void
grid_erase(struct grid *grid, int start, int end)
{
    assert(end >= start);
    memset(&grid->cells[start], 0, (end - start) * sizeof(grid->cells[0]));

    for (int i = start; i < end; i++) {
        struct cell *cell = &grid->cells[i];

        cell->attrs.foreground = grid->foreground;
        cell->attrs.background = grid->background;

        cell->dirty = true;
    }

    grid->dirty = true;
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
    grid_cursor_to(grid, grid->cursor.row, grid->cursor.col - move_amount);
}

void
grid_cursor_right(struct grid *grid, int count)
{
    int move_amount = min(grid->cols - grid->cursor.col - 1, count);
    grid_cursor_to(grid, grid->cursor.row, grid->cursor.col + move_amount);
}

void
grid_cursor_up(struct grid *grid, int count)
{
    int move_amount = min(grid->cursor.row, count);
    grid_cursor_to(grid, grid->cursor.row - move_amount, grid->cursor.col);
}

void
grid_cursor_down(struct grid *grid, int count)
{
    int move_amount = min(grid->rows - grid->cursor.row - 1, count);
    grid_cursor_to(grid, grid->cursor.row + move_amount, grid->cursor.col);
}

void
grid_scroll(struct grid *grid, int rows)
{
    if (rows >= grid->rows) {
        grid_erase(grid, 0, grid->rows * grid->cols);
        return;
    }

    int first_row = rows;
    int cell_start = first_row * grid->cols;
    int cell_end = grid->rows * grid->cols;
    int count = cell_end - cell_start;
    LOG_DBG("moving %d cells from %d", count, cell_start);

    memmove(&grid->cells[0], &grid->cells[cell_start], count * sizeof(grid->cells[0]));
    grid_erase(grid, cell_end - rows * grid->cols, grid->rows * grid->cols);
    grid->all_dirty = true;
}
