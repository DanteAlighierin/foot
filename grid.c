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
        return false;;

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

void
grid_damage_update(struct grid *grid, int start, int length)
{
    struct damage dmg = {
        .type = DAMAGE_UPDATE,
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
grid_damage_erase(struct grid *grid, int start, int length)
{
    struct damage dmg = {
        .type = DAMAGE_ERASE,
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

static void
damage_adjust_after_scroll(struct grid *grid, int top_margin,
                           int bottom_margin, int lines)
{
    const int adjustment = lines * grid->cols;
    tll_foreach(grid->damage, it) {
        if (it->item.type == DAMAGE_SCROLL)
            continue;

        assert(top_margin == 0 && "must check if item is in the non-scrolling region");

        it->item.range.start -= adjustment;

        if (it->item.range.start < 0) {
            int new_length = it->item.range.length + it->item.range.start;
            assert(new_length < it->item.range.length);

            if (new_length <= 0)
                tll_remove(grid->damage, it);
            else {
                it->item.range.length = new_length;
                it->item.range.start = 0;
            }
        }
    }
}

void
grid_damage_scroll(struct grid *grid, int top_margin, int bottom_margin,
                   int lines)
{
    if (tll_length(grid->damage) > 0 &&
        tll_front(grid->damage).type == DAMAGE_SCROLL)
    {
        /* Merge with existing scroll damage */

        struct damage *dmg = &tll_front(grid->damage);

        assert(dmg->scroll.top_margin == top_margin);
        assert(dmg->scroll.bottom_margin == bottom_margin);

        dmg->scroll.lines += lines;

        /* If we've scrolled away the entire screen, replace with an erase */
        if (dmg->scroll.lines >= (grid->rows - (dmg->scroll.top_margin +
                                                dmg->scroll.bottom_margin))) {
            dmg->type = DAMAGE_ERASE;
            dmg->range.start = dmg->scroll.top_margin * grid->cols;
            dmg->range.length = dmg->scroll.lines * grid->cols;
        }
    } else {
        /* TODO: yield a DAMAGE_ERASE if lines > screen */
        struct damage dmg = {
            .type = DAMAGE_SCROLL,
            .scroll = {
                .top_margin = top_margin,
                .bottom_margin = bottom_margin,
                .lines = lines
            },
        };
        tll_push_front(grid->damage, dmg);
    }

    damage_adjust_after_scroll(grid, top_margin, bottom_margin, lines);
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
grid_scroll(struct grid *grid, int rows)
{
    const int top_margin = 0;
    const int bottom_margin = 0;
    const int grid_rows = grid->rows - top_margin - bottom_margin;

    if (rows >= grid_rows) {
        assert(false && "need to take margins into account");
        grid_erase(grid, 0, grid->rows * grid->cols);
        return;
    }

    int cell_dst = top_margin * grid->cols;
    int cell_src = (top_margin + rows) * grid->cols;
    int cell_end = (grid->rows - bottom_margin) * grid->cols;
    int count = cell_end - cell_src;
    LOG_DBG("moving %d cells from %d", count, cell_src);

    const size_t bytes = count * sizeof(grid->cells[0]);
    memmove(
        &grid->cells[cell_dst], &grid->cells[cell_src],
        bytes);

    grid_damage_scroll(grid, top_margin, bottom_margin, rows);
    grid_erase(grid, cell_end - rows * grid->cols, grid->rows * grid->cols);
}
