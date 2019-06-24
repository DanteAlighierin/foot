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
    const int cols = grid->cols;

    if (length == 0)
        return;

    if (start < grid->scrolling_region.start * cols) {
        int len = min(length, grid->scrolling_region.start * cols);

        struct damage dmg = {
            .type = damage_type + 1,
            .range = {.start = start, .length = len},
        };

        tll_push_back(grid->damage, dmg);
        start += len;
        length -= len;
    }

    assert(start >= grid->scrolling_region.start * cols);

    if (length == 0)
        return;

    if (start + length > (grid->scrolling_region.end * cols)) {
        int partial_start = max(start, grid->scrolling_region.end * cols);
        int len = start + length - partial_start;

        struct damage dmg = {
            .type = damage_type + 1,
            .range = {.start = partial_start, .length = len},
        };

        tll_push_back(grid->damage, dmg);
        length -= len;
    }

    if (length == 0)
        return;

    assert(start < grid->scrolling_region.end * cols);
    assert(start + length <= grid->scrolling_region.end * cols);

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

static void
damage_adjust_after_scroll(struct grid *grid, enum damage_type damage_type,
                           int lines)
{
    int top_margin = grid->scrolling_region.start;
    int bottom_margin = grid->rows - grid->scrolling_region.end;

    const int adjustment
        = lines * grid->cols * (damage_type == DAMAGE_SCROLL_REVERSE ? -1 : 1);
    top_margin *= grid->cols;
    bottom_margin *= grid->cols;

    tll_foreach(grid->damage, it) {
        if (!(it->item.type == DAMAGE_UPDATE || it->item.type == DAMAGE_ERASE))
            continue;

        assert(it->item.range.start >= grid->scrolling_region.start * grid->cols);
        assert(it->item.range.start + it->item.range.length <=
               grid->scrolling_region.end * grid->cols);

        it->item.range.start -= adjustment;
        int end = it->item.range.start + it->item.range.length;

        if (damage_type == DAMAGE_SCROLL && it->item.range.start < top_margin) {
            /* Scrolled of screen (partially, or completely) */
            int new_length = it->item.range.length - (top_margin - it->item.range.start);
            assert(new_length < it->item.range.length);

            if (new_length <= 0)
                tll_remove(grid->damage, it);
            else {
                it->item.range.length = new_length;
                it->item.range.start = 0;
            }
        }

        else if (damage_type == DAMAGE_SCROLL_REVERSE &&
                 end >= (grid->rows * grid->cols - bottom_margin))
        {
            /* Scrolled of screen (partially, or completely) */
            if (it->item.range.start >= (grid->rows * grid->cols - bottom_margin))
                tll_remove(grid->damage, it);
            else
                it->item.range.length =
                    (grid->rows * grid->cols - bottom_margin) - it->item.range.start;
        }
    }
}

void
grid_damage_scroll(struct grid *grid, enum damage_type damage_type,
                   int offset, int lines)
{
    if (tll_length(grid->damage) > 0 &&
        tll_front(grid->damage).type == damage_type &&
        tll_front(grid->damage).scroll.offset == offset)
    {
        /* Merge with existing scroll damage */

        struct damage *dmg = &tll_front(grid->damage);
        dmg->scroll.lines += lines;

        const int scrolling_region =
            grid->scrolling_region.end - grid->scrolling_region.start;

        /* If we've scrolled away the entire screen, replace with an erase */
        if (dmg->scroll.lines >= scrolling_region - offset) {
            dmg->type = DAMAGE_ERASE;
            dmg->range.start = (grid->scrolling_region.start + offset) * grid->cols;
            dmg->range.length = (scrolling_region - offset) * grid->cols;
        }
    } else {
        struct damage dmg = {
            .type = damage_type,
            .scroll = {.offset = offset, .lines = lines},
        };
        tll_push_front(grid->damage, dmg);
    }

    damage_adjust_after_scroll(grid, damage_type, lines);
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
grid_scroll_partial(struct grid *grid, int offset, int rows)
{
    const int grid_rows = grid->scrolling_region.end - grid->scrolling_region.start;
    const int top_margin = grid->scrolling_region.start;
    //const int bottom_margin = grid->rows - grid->scrolling_region.end;

    if (rows >= grid_rows - offset) {
        assert(false && "untested");
        return;
    }

    int cell_dst = (top_margin + offset) * grid->cols;
    int cell_src = (top_margin + offset + rows) * grid->cols;
    int cell_count = (grid_rows - offset - rows) * grid->cols;

    LOG_DBG("moving %d lines from row %d to row %d", cell_count / grid->cols,
            cell_src / grid->cols, (top_margin + offset) / grid->cols);

    const size_t bytes = cell_count * sizeof(grid->cells[0]);
    memmove(
        &grid->cells[cell_dst], &grid->cells[cell_src],
        bytes);

    grid_damage_scroll(grid, DAMAGE_SCROLL, offset, rows);
    grid_erase(
        grid,
        (grid->scrolling_region.end - rows) * grid->cols,
        grid->scrolling_region.end * grid->cols);
}

void
grid_scroll(struct grid *grid, int rows)
{
    grid_scroll_partial(grid, 0, rows);
}

void
grid_scroll_reverse_partial(struct grid *grid, int offset, int rows)
{
    const int grid_rows = grid->scrolling_region.end - grid->scrolling_region.start;
    const int top_margin = grid->scrolling_region.start;

    if (rows >= grid_rows - offset) {
        assert(false && "todo");
        return;
    }

    int cell_dst = (top_margin + offset + rows) * grid->cols;
    int cell_src = (top_margin + offset) * grid->cols;
    int cell_count = (grid_rows - offset - rows) * grid->cols;

    LOG_DBG("moving %d lines from row %d to row %d", cell_count / grid->cols,
            cell_src / grid->cols, (top_margin + offset) / grid->cols);

    const size_t bytes = cell_count * sizeof(grid->cells[0]);
    memmove(
        &grid->cells[cell_dst], &grid->cells[cell_src],
        bytes);

    grid_damage_scroll(grid, DAMAGE_SCROLL_REVERSE, offset, rows);
    grid_erase(
        grid,
        (top_margin + offset) * grid->cols,
        (top_margin + offset + rows) * grid->cols);
}

void
grid_scroll_reverse(struct grid *grid, int rows)
{
    grid_scroll_reverse_partial(grid, 0, rows);
}
