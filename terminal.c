#include "terminal.h"

#include <string.h>
#include <assert.h>

#define LOG_MODULE "terminal"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "grid.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

static bool
damage_merge_range(struct terminal *term, const struct damage *dmg)
{
    if (tll_length(term->grid->damage) == 0)
        return false;

    struct damage *old = &tll_back(term->grid->damage);
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

        return true;
    }

    return false;
}

static void
term_damage_update_or_erase(struct terminal *term, enum damage_type damage_type,
                            int start, int length)
{
#if 1
    if (tll_length(term->grid->damage) > 1024) {
        term_damage_all(term);
        return;
    }
#endif

    struct damage dmg = {
        .type = damage_type,
        .range = {.start = term->grid->offset + start, .length = length},
    };

    if (damage_merge_range(term, &dmg))
        return;

    tll_push_back(term->grid->damage, dmg);
}

void
term_damage_update(struct terminal *term, int start, int length)
{
    assert(start + length <= term->rows * term->cols);
    term_damage_update_or_erase(term, DAMAGE_UPDATE, start, length);
}

void
term_damage_erase(struct terminal *term, int start, int length)
{
    assert(start + length <= term->rows * term->cols);
    term_damage_update_or_erase(term, DAMAGE_ERASE, start, length);
}

void
term_damage_all(struct terminal *term)
{
    tll_free(term->grid->damage);
    tll_free(term->grid->scroll_damage);
    term_damage_update(term, 0, term->rows * term->cols);
}

#if 0
static void
damage_adjust_after_scroll(struct terminal *term, enum damage_type damage_type,
                           struct scroll_region region, int lines)
{
    tll_foreach(term->grid->damage, it) {
#if 0
        if (it->item.range.start < term->grid->offset) {
            int end = it->item.range.start + it->item.range.length;
            if (end >= term->grid->offset) {
                it->item.range.start = term->grid->offset;
                it->item.range.length = end - it->item.range.start;
            } else {
                tll_remove(term->grid->damage, it);
            }
        }
#endif

        int start = it->item.range.start;
        int end = start + it->item.range.length;

        if (start - 
    }
}
#endif

void
term_damage_scroll(struct terminal *term, enum damage_type damage_type,
                   struct scroll_region region, int lines)
{
    //damage_adjust_after_scroll(term, damage_type, region, lines);
    if (damage_type == DAMAGE_SCROLL) {
        tll_foreach(term->grid->damage, it) {
            int start = it->item.range.start;
            int length = it->item.range.length;

            if (start < term->grid->offset) {
                int end = start + length;
                if (end >= term->grid->offset) {
                    it->item.range.start = term->grid->offset;
                    it->item.range.length = end - it->item.range.start;
                } else
                    tll_remove(term->grid->damage, it);
            } else
                break;
        }
    }

    if (tll_length(term->grid->scroll_damage) > 0) {
        struct damage *dmg = &tll_back(term->grid->scroll_damage);

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
    tll_push_back(term->grid->scroll_damage, dmg);
}

void
term_erase(struct terminal *term, int start, int end)
{
    LOG_DBG("erase: %d-%d", start, end);
    assert(end >= start);
    assert(end <= term->rows * term->cols);

    if (term->vt.attrs.have_background) {
        int erase_start = start;
        int left = end - erase_start;

        while (left > 0) {
            int len = left;
            struct cell *cells = grid_get_range(term->grid, erase_start, &len);

            memset(cells, 0, len * sizeof(cells[0]));

            for (int i = 0; i < len; i++) {
                cells[i].attrs.background = term->vt.attrs.background;
                cells[i].attrs.have_background = true;
            }

            erase_start += len;
            left -= len;
        }

        term_damage_update(term, start, end - start);
    } else {
        grid_memset(term->grid, start, 0, end - start);
        term_damage_erase(term, start, end - start);
    }
}

int
term_cursor_linear(const struct terminal *term, int row, int col)
{
    return row * term->cols + col;
}

void
term_cursor_to(struct terminal *term, int row, int col)
{
    assert(row < term->rows);
    assert(col < term->cols);

    int new_linear = row * term->cols + col;
    assert(new_linear < term->rows * term->cols);

    term->print_needs_wrap = false;

    term->cursor.linear = new_linear;
    term->cursor.col = col;
    term->cursor.row = row;

    int len = term->cols;
    term->grid->cur_line = grid_get_range(
        term->grid, term->cursor.linear - col, &len);

    assert(len == term->cols);
}

void
term_cursor_left(struct terminal *term, int count)
{
    int move_amount = min(term->cursor.col, count);
    term->cursor.linear -= move_amount;
    term->cursor.col -= move_amount;
    term->print_needs_wrap = false;
}

void
term_cursor_right(struct terminal *term, int count)
{
    int move_amount = min(term->cols - term->cursor.col - 1, count);
    term->cursor.linear += move_amount;
    term->cursor.col += move_amount;
    term->print_needs_wrap = false;
}

void
term_cursor_up(struct terminal *term, int count)
{
    int move_amount = min(term->cursor.row, count);
    term_cursor_to(term, term->cursor.row - move_amount, term->cursor.col);
}

void
term_cursor_down(struct terminal *term, int count)
{
    int move_amount = min(term->rows - term->cursor.row - 1, count);
    term_cursor_to(term, term->cursor.row + move_amount, term->cursor.col);
}

void
term_scroll_partial(struct terminal *term, struct scroll_region region, int rows)
{
    LOG_DBG("scroll: %d rows", rows);

    if (region.start > 0) {
        /* TODO: check if it's worth memoving the scroll area instead,
         * under certain circumstances */

        grid_memmove(term->grid, rows * term->cols, 0, region.start * term->cols);

        tll_foreach(term->grid->damage, it) {
            int start = it->item.range.start - term->grid->offset;
            int end __attribute__((unused))  = start + it->item.range.length;

            if (start < region.start * term->cols) {
                assert(end <= region.start * term->cols);
                it->item.range.start += rows * term->cols;
            }
        }
    }

    if (region.end < term->rows) {
        /* Copy scrolled-up bottom region to new bottom region */
        grid_memmove(
            term->grid,
            (region.end + rows) * term->cols,
            region.end * term->cols,
            (term->rows - region.end) * term->cols);

        tll_foreach(term->grid->damage, it) {
            int start = it->item.range.start - term->grid->offset;
            int end = start + it->item.range.length;

            if (end > region.end * term->cols) {
                assert(start >= region.end * term->cols);
                it->item.range.start += rows * term->cols;
            }
        }
    }

    /* Offset grid origin */
    term->grid->offset += rows * term->cols;

    /* Clear scrolled-in lines */
    grid_memset(
        term->grid,
        max(0, region.end - rows) * term->cols,
        0,
        min(rows, term->rows) * term->cols);

    term_damage_scroll(term, DAMAGE_SCROLL, region, rows);

    int len = term->cols;
    term->grid->cur_line = grid_get_range(
        term->grid, term->cursor.linear - term->cursor.col, &len);

    assert(len == term->cols);
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
    if (region.end < term->rows) {
        grid_memmove(
            term->grid,
            (region.end - rows) * term->cols,
            region.end * term->cols,
            (term->rows - region.end) * term->cols);

        tll_foreach(term->grid->damage, it) {
            int start = it->item.range.start - term->grid->offset;
            int end = start + it->item.range.length;

            if (end > region.end * term->cols) {
                assert(start >= region.end * term->cols);
                it->item.range.start -= rows * term->cols;
            }
        }

    }

    if (region.start > 0) {
        grid_memmove(
            term->grid, -rows * term->cols, 0, region.start * term->cols);

        tll_foreach(term->grid->damage, it) {
            int start = it->item.range.start - term->grid->offset;
            int end __attribute__((unused))  = start + it->item.range.length;

            if (start < region.start * term->cols) {
                if (end > region.start * term->cols) {
                    LOG_ERR("region.start = %d, rows = %d, damage.start = %d, damage.end = %d (%s)",
                            region.start, rows, start, end, it->item.type == DAMAGE_UPDATE ? "UPDATE" : "ERASE");
                    abort();
                }
                assert(end <= region.start * term->cols);
                it->item.range.start -= rows * term->cols;
            }
        }
    }

    term->grid->offset -= rows * term->cols;

    grid_memset(term->grid, region.start * term->cols, 0, rows * term->cols);

    term_damage_scroll(term, DAMAGE_SCROLL_REVERSE, region, rows);

    int len = term->cols;
    term->grid->cur_line = grid_get_range(
        term->grid, term->cursor.linear - term->cursor.col, &len);

    assert(len == term->cols);
}

void
term_scroll_reverse(struct terminal *term, int rows)
{
    term_scroll_reverse_partial(term, term->scroll_region, rows);
}
