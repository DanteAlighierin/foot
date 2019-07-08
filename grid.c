#include "grid.h"

#include <string.h>
#include <assert.h>

#define LOG_MODULE "grid"
#define LOG_ENABLE_DBG 0
#include "log.h"
#if 0
struct cell *
grid_get_range(struct grid *grid, int start, int *length)
{
#define min(x, y) ((x) < (y) ? (x) : (y))
    assert(*length <= grid->size);

    int real_start = (grid->offset + start) % grid->size;
    if (real_start < 0)
        real_start += grid->size;
    assert(real_start >= 0);
    assert(real_start < grid->size);

    *length = min(*length, grid->size - real_start);
    assert(real_start + *length <= grid->size);

    return &grid->cells[real_start];
#undef min
}

void
grid_memclear(struct grid *grid, int start, int length)
{
    int left = length;
    while (left > 0) {
        int count = left;
        struct cell *cells = grid_get_range(grid, start, &count);

        assert(count > 0);
        assert(count <= left);

        memset(cells, 0, count * sizeof(cells[0]));

        left -= count;
        start += count;
    }
}

void
grid_memmove(struct grid *grid, int dst, int src, int length)
{
    /* Fast path, we can move everything in one swoop */
    {
        int count = length;
        struct cell *dst_cells = grid_get_range(grid, dst, &count);
        if (count == length) {
            struct cell *src_cells = grid_get_range(grid, src, &count);
            if (count == length) {
                memmove(dst_cells, src_cells, length * sizeof(dst_cells[0]));
                return;
            }
        }
    }

    int left = length;
    int copy_idx = 0;
    struct cell copy[left];

    while (left > 0) {
        int count = left;
        struct cell *src_cells = grid_get_range(grid, src, &count);

        memcpy(&copy[copy_idx], src_cells, count * sizeof(copy[0]));

        left -= count;
        src += count;
        copy_idx += count;
    }

    left = length;
    copy_idx = 0;

    while (left > 0) {
        int count = left;
        struct cell *dst_cells = grid_get_range(grid, dst, &count);

        memcpy(dst_cells, &copy[copy_idx], count * sizeof(copy[0]));

        left -= count;
        dst += count;
        copy_idx += count;
    }
}
#endif
