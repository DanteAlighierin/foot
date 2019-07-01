#include "grid.h"

#include <string.h>
#include <assert.h>

#define LOG_MODULE "grid"
#define LOG_ENABLE_DBG 1
#include "log.h"

struct cell *
grid_get_range(struct grid *grid, size_t start, size_t *length)
{
#define min(x, y) ((x) < (y) ? (x) : (y))
    assert(*length <= grid->size);

    size_t real_start = (grid->offset + start) % grid->size;
    assert(real_start < grid->size);

    *length = min(*length, grid->size - real_start);
    assert(real_start + *length <= grid->size);

    return &grid->cells[real_start];
#undef min
}

void
grid_memset(struct grid *grid, size_t start, int c, size_t length)
{
    size_t left = length;
    while (left > 0) {
        size_t count = left;
        struct cell *cells = grid_get_range(grid, start, &count);

        assert(count > 0);
        assert(count <= left);

        memset(cells, c, count * sizeof(cells[0]));

        left -= count;
        start += count;
    }
}

void
grid_memmove(struct grid *grid, size_t dst, size_t src, size_t length)
{
    size_t left = length;
    size_t copy_idx = 0;
    struct cell copy[left];

    while (left > 0) {
        size_t count = left;
        struct cell *src_cells = grid_get_range(grid, src, &count);

        memcpy(&copy[copy_idx], src_cells, count * sizeof(copy[0]));

        left -= count;
        src += count;
        copy_idx += count;
    }

    left = length;
    copy_idx = 0;

    while (left > 0) {
        size_t count = left;
        struct cell *dst_cells = grid_get_range(grid, dst, &count);

        memcpy(dst_cells, &copy[copy_idx], count * sizeof(copy[0]));

        left -= count;
        dst += count;
        copy_idx += count;
    }
}
