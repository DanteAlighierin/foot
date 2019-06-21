#pragma once

#include "terminal.h"

void grid_damage_update(struct grid *grid, int start, int length);
void grid_damage_erase(struct grid *grid, int start, int length);
void grid_damage_scroll(
    struct grid *grid, int top_margin, int bottom_margin, int lines);

void grid_erase(struct grid *grid, int start, int end);

void grid_cursor_to(struct grid *grid, int row, int col);
void grid_cursor_left(struct grid *grid, int count);
void grid_cursor_right(struct grid *grid, int count);
void grid_cursor_up(struct grid *grid, int count);
void grid_cursor_down(struct grid *grid, int count);

void grid_scroll(struct grid *grid, int rows);

int grid_cursor_linear(const struct grid *grid, int row, int col);


//void grid_cursor_to(struct grid *grid, int pos);
//void grid_cursor_move(struct grid *grid, int cols);
