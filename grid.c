#include "grid.h"

#include <string.h>

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

void
grid_cursor_move(struct grid *grid, int cols)
{
    int new_cursor = grid->cursor + cols;
    grid->cells[grid->cursor].dirty = true;
    grid->cells[new_cursor].dirty = true;
    grid->cursor = new_cursor;
    grid->dirty = true;
    grid->print_needs_wrap = false;
}

void
grid_cursor_to(struct grid *grid, int pos)
{
    grid_cursor_move(grid, pos - grid->cursor);
}
