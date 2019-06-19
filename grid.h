#pragma once

#include "terminal.h"

void grid_erase(struct grid *grid, int start, int end);

void grid_cursor_to(struct grid *grid, int row, int col);
void grid_cursor_left(struct grid *grid, int count);
void grid_cursor_right(struct grid *grid, int count);
void grid_cursor_up(struct grid *grid, int count);
void grid_cursor_down(struct grid *grid, int count);

int grid_cursor_linear(const struct grid *grid, int row, int col);

//void grid_cursor_to(struct grid *grid, int pos);
//void grid_cursor_move(struct grid *grid, int cols);
