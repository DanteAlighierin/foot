#include "grid.h"

#include <string.h>
#include <assert.h>

#define LOG_MODULE "grid"
#define LOG_ENABLE_DBG 1
#include "log.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

static bool
damage_merge_range(struct grid *grid, const struct damage *dmg)
{
    if (tll_length(grid->damage) == 0)
        return false;

    struct damage *old = &tll_back(grid->damage);
    if (old->type != dmg->type)
        return false;

    const int start = dmg->range.start;
    const int end = start + dmg->range.length;

    const int prev_start = old->range.start;
    const int prev_end = prev_start + old->range.length;

    if ((start >= prev_start && start <= prev_end) ||
        (end >= prev_start && end <= prev_end) ||
        (start <= prev_start && end >= prev_end))
    {
        /* The two damage ranges intersect */
        int new_start = min(start, prev_start);
        int new_end = max(end, prev_end);

        old->range.start = new_start;
        old->range.length = new_end - new_start;

        assert(old->range.start >= 0);
        assert(old->range.start < grid->rows * grid->cols);
        assert(old->range.length >= 0);
        assert(old->range.start + old->range.length <= grid->rows * grid->cols);
        return true;
    }

    return false;
}

static void
grid_damage_update_or_erase(struct grid *grid, enum damage_type damage_type,
                            int start, int length)
{
    struct damage dmg = {
        .type = damage_type,
        .range = {.start = start, .length = length},
    };

    assert(dmg.range.start >= 0);
    assert(dmg.range.start < grid->rows * grid->cols);
    assert(dmg.range.length >= 0);
    assert(dmg.range.start + dmg.range.length <= grid->rows * grid->cols);

    if (damage_merge_range(grid, &dmg))
        return;

    tll_push_back(grid->damage, dmg);
}

void
grid_damage_update(struct grid *grid, int start, int length)
{
    grid_damage_update_or_erase(grid, DAMAGE_UPDATE, start, length);
}

void
grid_damage_erase(struct grid *grid, int start, int length)
{
    grid_damage_update_or_erase(grid, DAMAGE_ERASE, start, length);
}

void
grid_damage_all(struct grid *grid)
{
    tll_free(grid->damage);
    tll_free(grid->scroll_damage);
    grid_damage_update(grid, 0, grid->rows * grid->cols);
}

static void
damage_adjust_after_scroll(struct grid *grid, enum damage_type damage_type,
                           struct scroll_region region, int lines)
{
    const int adjustment
        = lines * grid->cols * (damage_type == DAMAGE_SCROLL_REVERSE ? -1 : 1);

    const int scroll_start = region.start * grid->cols;
    const int scroll_end = region.end * grid->cols;

    tll_foreach(grid->damage, it) {
        int start = it->item.range.start;
        int length = it->item.range.length;
        int end = start + length;

        if (start < scroll_start && end > scroll_start) {
            /* Start outside, end either inside or on the other side */
            struct damage outside = {
                .type = it->item.type,
                .range = {.start = start, .length = scroll_start - start},
            };

            tll_push_back(grid->damage, outside);
            start = scroll_start;
            length = end - start;

        }

        if (start < scroll_end && end > scroll_end) {
            /* End outside, start either inside or on the other side */
            struct damage outside = {
                .type = it->item.type,
                .range = {.start = scroll_end, .length = length - scroll_end},
            };

            tll_push_back(grid->damage, outside);
            end = scroll_end;
            length = end - start;
        }

        if (start >= scroll_start && end <= scroll_end) {
            /* Completely inside scroll region */
            start -= adjustment;
            it->item.range.start = start;

            if (start < scroll_start) {
                /* Scrolled up outside scroll region */
                int new_length = length - (scroll_start - start);
                assert(new_length < length);

                if (new_length <= 0)
                    tll_remove(grid->damage, it);
                else {
                    it->item.range.start = scroll_start;
                    it->item.range.length = new_length;
                }
            }

            if (start + length > scroll_end) {
                /* Scrolled down outside scroll region */
                if (start >= scroll_end)
                    tll_remove(grid->damage, it);
                else {
                    it->item.range.start = start;
                    it->item.range.length = scroll_end - start;
                }
            }
        }
    }
}

void
grid_damage_scroll(struct grid *grid, enum damage_type damage_type,
                   struct scroll_region region, int lines)
{
    damage_adjust_after_scroll(grid, damage_type, region, lines);

    if (tll_length(grid->scroll_damage) > 0) {
        struct damage *dmg = &tll_back(grid->scroll_damage);

        if (dmg->type == damage_type &&
            dmg->scroll.region.start == region.start &&
            dmg->scroll.region.end == region.end)
        {
            dmg->scroll.lines += lines;
            return;
        }
    }
    struct damage dmg = {
        .type = damage_type,
        .scroll = {.region = region, .lines = lines},
    };
    tll_push_back(grid->scroll_damage, dmg);
}

void
grid_erase(struct grid *grid, int start, int end)
{
    assert(end >= start);
    memset(&grid->cells[start], 0, (end - start) * sizeof(grid->cells[0]));

    for (int i = start; i < end; i++) {
        struct cell *cell = &grid->cells[i];

        cell->attrs.foreground = grid->foreground;
        cell->attrs.background = grid->background;
    }

    grid_damage_erase(grid, start, end - start);
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

    grid_damage_update(grid, grid->linear_cursor, 1);
    grid_damage_update(grid, new_linear, 1);
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
grid_scroll_partial(struct grid *grid, struct scroll_region region, int rows)
{
    if (rows >= region.end - region.start) {
        assert(false && "untested");
        return;
    }

    int cell_dst = (region.start + 0) * grid->cols;
    int cell_src = (region.start + rows) * grid->cols;
    int cell_count = (region.end - region.start - rows) * grid->cols;

    LOG_DBG("moving %d lines from row %d to row %d", cell_count / grid->cols,
            cell_src / grid->cols, cell_dst / grid->cols);

    const size_t bytes = cell_count * sizeof(grid->cells[0]);
    memmove(
        &grid->cells[cell_dst], &grid->cells[cell_src],
        bytes);

    memset(&grid->cells[(region.end - rows) * grid->cols], 0,
           rows * grid->cols * sizeof(grid->cells[0]));

    grid_damage_scroll(grid, DAMAGE_SCROLL, region, rows);
}

void
grid_scroll(struct grid *grid, int rows)
{
    grid_scroll_partial(grid, grid->scroll_region, rows);
}

void
grid_scroll_reverse_partial(struct grid *grid,
                            struct scroll_region region, int rows)
{
    if (rows >= region.end - region.start) {
        assert(false && "todo");
        return;
    }

    int cell_dst = (region.start + rows) * grid->cols;
    int cell_src = (region.start + 0) * grid->cols;
    int cell_count = (region.end - region.start - rows) * grid->cols;

    LOG_DBG("moving %d lines from row %d to row %d", cell_count / grid->cols,
            cell_src / grid->cols, cell_dst / grid->cols);

    const size_t bytes = cell_count * sizeof(grid->cells[0]);
    memmove(
        &grid->cells[cell_dst], &grid->cells[cell_src],
        bytes);

    memset(&grid->cells[cell_src], 0,
           rows * grid->cols * sizeof(grid->cells[0]));

    grid_damage_scroll(grid, DAMAGE_SCROLL_REVERSE, region, rows);
}

void
grid_scroll_reverse(struct grid *grid, int rows)
{
    grid_scroll_reverse_partial(grid, grid->scroll_region, rows);
}
