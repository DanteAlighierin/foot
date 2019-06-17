#pragma once

#include "terminal.h"

void grid_erase(struct grid *grid, int start, int end);

void grid_cursor_to(struct grid *grid, int pos);
void grid_cursor_move(struct grid *grid, int cols);
