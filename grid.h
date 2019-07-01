#pragma once

#include <stddef.h>
#include "terminal.h"

struct cell *grid_get_range(struct grid *grid, size_t start, size_t *length);
void grid_memset(struct grid *grid, size_t start, int c, size_t length);
