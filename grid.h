#pragma once

#include <stddef.h>
#include "terminal.h"

struct cell *grid_get_range(struct grid *grid, int start, int *length);
void grid_memset(struct grid *grid, int start, int c, int length);
void grid_memmove(struct grid *grid, int dst, int src, int length);
