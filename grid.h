#pragma once

#include "terminal.h"

void grid_damage_all(struct grid *grid);
void grid_damage_update(struct grid *grid, int start, int length);
void grid_damage_erase(struct grid *grid, int start, int length);
void grid_damage_scroll(
    struct grid *grid, enum damage_type damage_type,
    struct scroll_region region, int lines);

void grid_erase(struct grid *grid, int start, int end);

void grid_cursor_to(struct grid *grid, int row, int col);
void grid_cursor_left(struct grid *grid, int count);
void grid_cursor_right(struct grid *grid, int count);
void grid_cursor_up(struct grid *grid, int count);
void grid_cursor_down(struct grid *grid, int count);

void grid_scroll(struct grid *grid, int rows);
void grid_scroll_reverse(struct grid *grid, int rows);

void grid_scroll_partial(
    struct grid *grid, struct scroll_region region, int rows);
void grid_scroll_reverse_partial(
    struct grid *grid, struct scroll_region region, int rows);

int grid_cursor_linear(const struct grid *grid, int row, int col);


//void grid_cursor_to(struct grid *grid, int pos);
//void grid_cursor_move(struct grid *grid, int cols);
