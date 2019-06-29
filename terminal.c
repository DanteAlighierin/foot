#include "terminal.h"

#include <string.h>
#include <assert.h>

#define LOG_MODULE "terminal"
#define LOG_ENABLE_DBG 1
#include "log.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

static bool
damage_merge_range(struct terminal *term, const struct damage *dmg)
{
    if (tll_length(term->grid.damage) == 0)
        return false;

    struct damage *old = &tll_back(term->grid.damage);
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
        assert(old->range.start < term->rows * term->cols);
        assert(old->range.length >= 0);
        assert(old->range.start + old->range.length <= term->rows * term->cols);
        return true;
    }

    return false;
}

static void
term_damage_update_or_erase(struct terminal *term, enum damage_type damage_type,
                            int start, int length)
{
    struct damage dmg = {
        .type = damage_type,
        .range = {.start = start, .length = length},
    };

    assert(dmg.range.start >= 0);
    assert(dmg.range.start < term->rows * term->cols);
    assert(dmg.range.length >= 0);
    assert(dmg.range.start + dmg.range.length <= term->rows * term->cols);

    if (damage_merge_range(term, &dmg))
        return;

    tll_push_back(term->grid.damage, dmg);
}

void
term_damage_update(struct terminal *term, int start, int length)
{
    term_damage_update_or_erase(term, DAMAGE_UPDATE, start, length);
}

void
term_damage_erase(struct terminal *term, int start, int length)
{
    term_damage_update_or_erase(term, DAMAGE_ERASE, start, length);
}

void
term_damage_all(struct terminal *term)
{
    tll_free(term->grid.damage);
    tll_free(term->grid.scroll_damage);
    term_damage_update(term, 0, term->rows * term->cols);
}

static void
damage_adjust_after_scroll(struct terminal *term, enum damage_type damage_type,
                           struct scroll_region region, int lines)
{
    const int adjustment
        = lines * term->cols * (damage_type == DAMAGE_SCROLL_REVERSE ? -1 : 1);

    const int scroll_start = region.start * term->cols;
    const int scroll_end = region.end * term->cols;

    tll_foreach(term->grid.damage, it) {
        int start = it->item.range.start;
        int length = it->item.range.length;
        int end = start + length;

        if (start < scroll_start && end > scroll_start) {
            /* Start outside, end either inside or on the other side */
            struct damage outside = {
                .type = it->item.type,
                .range = {.start = start, .length = scroll_start - start},
            };

            tll_push_back(term->grid.damage, outside);
            start = scroll_start;
            length = end - start;

        }

        if (start < scroll_end && end > scroll_end) {
            /* End outside, start either inside or on the other side */
            struct damage outside = {
                .type = it->item.type,
                .range = {.start = scroll_end, .length = length - scroll_end},
            };

            tll_push_back(term->grid.damage, outside);
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
                    tll_remove(term->grid.damage, it);
                else {
                    it->item.range.start = scroll_start;
                    it->item.range.length = new_length;
                }
            }

            if (start + length > scroll_end) {
                /* Scrolled down outside scroll region */
                if (start >= scroll_end)
                    tll_remove(term->grid.damage, it);
                else {
                    it->item.range.start = start;
                    it->item.range.length = scroll_end - start;
                }
            }
        }
    }
}

void
term_damage_scroll(struct terminal *term, enum damage_type damage_type,
                   struct scroll_region region, int lines)
{
    damage_adjust_after_scroll(term, damage_type, region, lines);

    if (tll_length(term->grid.scroll_damage) > 0) {
        struct damage *dmg = &tll_back(term->grid.scroll_damage);

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
    tll_push_back(term->grid.scroll_damage, dmg);
}

void
term_erase(struct terminal *term, int start, int end)
{
    assert(end >= start);
    memset(&term->grid.cells[start], 0, (end - start) * sizeof(term->grid.cells[0]));
    term_damage_erase(term, start, end - start);
}

int
term_cursor_linear(const struct terminal *term, int row, int col)
{
    return row * term->cols + col;
}

void
term_cursor_to(struct terminal *term, int row, int col)
{
    assert(row >= 0);
    assert(row < term->rows);
    assert(col >= 0);
    assert(col < term->cols);

    int new_linear = row * term->cols + col;
    assert(new_linear >= 0);
    assert(new_linear < term->rows * term->cols);

    term_damage_update(term, term->grid.linear_cursor, 1);
    term_damage_update(term, new_linear, 1);
    term->grid.print_needs_wrap = false;

    term->grid.linear_cursor = new_linear;
    term->grid.cursor.col = col;
    term->grid.cursor.row = row;
}

void
term_cursor_left(struct terminal *term, int count)
{
    int move_amount = min(term->grid.cursor.col, count);
    term_cursor_to(term, term->grid.cursor.row, term->grid.cursor.col - move_amount);
}

void
term_cursor_right(struct terminal *term, int count)
{
    int move_amount = min(term->cols - term->grid.cursor.col - 1, count);
    term_cursor_to(term, term->grid.cursor.row, term->grid.cursor.col + move_amount);
}

void
term_cursor_up(struct terminal *term, int count)
{
    int move_amount = min(term->grid.cursor.row, count);
    term_cursor_to(term, term->grid.cursor.row - move_amount, term->grid.cursor.col);
}

void
term_cursor_down(struct terminal *term, int count)
{
    int move_amount = min(term->rows - term->grid.cursor.row - 1, count);
    term_cursor_to(term, term->grid.cursor.row + move_amount, term->grid.cursor.col);
}

void
term_scroll_partial(struct terminal *term, struct scroll_region region, int rows)
{
    if (rows >= region.end - region.start) {
        assert(false && "untested");
        return;
    }

    int cell_dst = (region.start + 0) * term->cols;
    int cell_src = (region.start + rows) * term->cols;
    int cell_count = (region.end - region.start - rows) * term->cols;

    LOG_DBG("moving %d lines from row %d to row %d", cell_count / term->cols,
            cell_src / term->cols, cell_dst / term->cols);

    const size_t bytes = cell_count * sizeof(term->grid.cells[0]);
    memmove(
        &term->grid.cells[cell_dst], &term->grid.cells[cell_src],
        bytes);

    memset(&term->grid.cells[(region.end - rows) * term->cols], 0,
           rows * term->cols * sizeof(term->grid.cells[0]));

    term_damage_scroll(term, DAMAGE_SCROLL, region, rows);
}

void
term_scroll(struct terminal *term, int rows)
{
    term_scroll_partial(term, term->scroll_region, rows);
}

void
term_scroll_reverse_partial(struct terminal *term,
                            struct scroll_region region, int rows)
{
    if (rows >= region.end - region.start) {
        assert(false && "todo");
        return;
    }

    int cell_dst = (region.start + rows) * term->cols;
    int cell_src = (region.start + 0) * term->cols;
    int cell_count = (region.end - region.start - rows) * term->cols;

    LOG_DBG("moving %d lines from row %d to row %d", cell_count / term->cols,
            cell_src / term->cols, cell_dst / term->cols);

    const size_t bytes = cell_count * sizeof(term->grid.cells[0]);
    memmove(
        &term->grid.cells[cell_dst], &term->grid.cells[cell_src],
        bytes);

    memset(&term->grid.cells[cell_src], 0,
           rows * term->cols * sizeof(term->grid.cells[0]));

    term_damage_scroll(term, DAMAGE_SCROLL_REVERSE, region, rows);
}

void
term_scroll_reverse(struct terminal *term, int rows)
{
    term_scroll_reverse_partial(term, term->scroll_region, rows);
}
