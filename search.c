#include "search.h"

#include <string.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "search"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "config.h"
#include "extract.h"
#include "grid.h"
#include "input.h"
#include "key-binding.h"
#include "misc.h"
#include "render.h"
#include "selection.h"
#include "shm.h"
#include "util.h"
#include "xmalloc.h"

/*
 * Ensures a "new" viewport doesn't contain any unallocated rows.
 *
 * This is done by first checking if the *first* row is NULL. If so,
 * we move the viewport *forward*, until the first row is non-NULL. At
 * this point, the entire viewport should be allocated rows only.
 *
 * If the first row already was non-NULL, we instead check the *last*
 * row, and if it is NULL, we move the viewport *backward* until the
 * last row is non-NULL.
 */
static int
ensure_view_is_allocated(struct terminal *term, int new_view)
{
    struct grid *grid = term->grid;
    int view_end = (new_view + term->rows - 1) & (grid->num_rows - 1);

    if (grid->rows[new_view] == NULL) {
        while (grid->rows[new_view] == NULL)
            new_view = (new_view + 1) & (grid->num_rows - 1);
    }

    else if (grid->rows[view_end] == NULL) {
        while (grid->rows[view_end] == NULL) {
            new_view--;
            if (new_view < 0)
                new_view += grid->num_rows;
            view_end = (new_view + term->rows - 1) & (grid->num_rows - 1);
        }
    }

#if defined(_DEBUG)
    for (size_t r = 0; r < term->rows; r++)
        xassert(grid->rows[(new_view + r) & (grid->num_rows - 1)] != NULL);
#endif

    return new_view;
}

static bool
search_ensure_size(struct terminal *term, size_t wanted_size)
{
    while (wanted_size >= term->search.sz) {
        size_t new_sz = term->search.sz == 0 ? 64 : term->search.sz * 2;
        char32_t *new_buf = realloc(term->search.buf, new_sz * sizeof(term->search.buf[0]));

        if (new_buf == NULL) {
            LOG_ERRNO("failed to resize search buffer");
            return false;
        }

        term->search.buf = new_buf;
        term->search.sz = new_sz;
    }

    return true;
}

static bool
has_wrapped_around(const struct terminal *term, int abs_row_no)
{
    int rebased_row = grid_row_abs_to_sb(term->grid, term->rows, abs_row_no);
    return rebased_row == 0;
}

static void
search_cancel_keep_selection(struct terminal *term)
{
    struct wl_window *win = term->window;
    wayl_win_subsurface_destroy(&win->search);

    if (term->search.len > 0) {
        free(term->search.last.buf);
        term->search.last.buf = term->search.buf;
        term->search.last.len = term->search.len;
    } else
        free(term->search.buf);

    term->search.buf = NULL;
    term->search.len = term->search.sz = 0;

    term->search.cursor = 0;
    term->search.match = (struct coord){-1, -1};
    term->search.match_len = 0;
    term->is_searching = false;
    term->render.search_glyph_offset = 0;

    /* Reset IME state */
    if (term_ime_is_enabled(term)) {
        term_ime_disable(term);
        term_ime_enable(term);
    }

    term_xcursor_update(term);
    render_refresh(term);

    /* Work around Sway bug - unmapping a sub-surface does not damage
     * the underlying surface */
    term_damage_margins(term);
    term_damage_view(term);
}

void
search_begin(struct terminal *term)
{
    LOG_DBG("search: begin");

    search_cancel_keep_selection(term);
    selection_cancel(term);

    /* Reset IME state */
    if (term_ime_is_enabled(term)) {
        term_ime_disable(term);
        term_ime_enable(term);
    }

    /* On-demand instantiate wayland surface */
    bool ret = wayl_win_subsurface_new(
        term->window, &term->window->search, false);
    xassert(ret);

    const struct grid *grid = term->grid;
    term->search.original_view = grid->view;
    term->search.view_followed_offset = grid->view == grid->offset;
    term->is_searching = true;

    term->search.len = 0;
    term->search.sz = 64;
    term->search.buf = xmalloc(term->search.sz * sizeof(term->search.buf[0]));
    term->search.buf[0] = U'\0';

    term_xcursor_update(term);
    render_refresh_search(term);
}

void
search_cancel(struct terminal *term)
{
    if (!term->is_searching)
        return;

    search_cancel_keep_selection(term);
    selection_cancel(term);
}

void
search_selection_cancelled(struct terminal *term)
{
    term->search.match = (struct coord){-1, -1};
    term->search.match_len = 0;
    render_refresh_search(term);
}

static void
search_update_selection(struct terminal *term, const struct range *match)
{
    struct grid *grid = term->grid;
    int start_row = match->start.row;
    int start_col = match->start.col;
    int end_row = match->end.row;
    int end_col = match->end.col;

    xassert(start_row >= 0);
    xassert(start_row < grid->num_rows);

    bool move_viewport = true;

    int view_end = (grid->view + term->rows - 1) & (grid->num_rows - 1);
    if (view_end >= grid->view) {
        /* Viewport does *not* wrap around */
        if (start_row >= grid->view && end_row <= view_end)
            move_viewport = false;
    } else {
        /* Viewport wraps */
        if (start_row >= grid->view || end_row <= view_end)
            move_viewport = false;
    }

    if (move_viewport) {
        int rebased_new_view = grid_row_abs_to_sb(grid, term->rows, start_row);

        rebased_new_view -= term->rows / 2;
        rebased_new_view =
            min(max(rebased_new_view, 0), grid->num_rows - term->rows);

        const int old_view = grid->view;
        int new_view = grid_row_sb_to_abs(grid, term->rows, rebased_new_view);

        /* Scrollback may not be completely filled yet */
        {
            const int mask = grid->num_rows - 1;
            while (grid->rows[new_view] == NULL)
                new_view = (new_view + 1) & mask;
        }

#if defined(_DEBUG)
        /* Verify all to-be-visible rows have been allocated */
        for (int r = 0; r < term->rows; r++)
            xassert(grid->rows[(new_view + r) & (grid->num_rows - 1)] != NULL);
#endif

#if defined(_DEBUG)
        {
            int rel_start_row = grid_row_abs_to_sb(grid, term->rows, start_row);
            int rel_view = grid_row_abs_to_sb(grid, term->rows, new_view);
            xassert(rel_view <= rel_start_row);
            xassert(rel_start_row < rel_view + term->rows);
        }
#endif

        /* Update view */
        grid->view = new_view;
        if (new_view != old_view)
            term_damage_view(term);
    }

    if (start_row != term->search.match.row ||
        start_col != term->search.match.col)
    {
        int selection_row = start_row - grid->view + grid->num_rows;
        selection_row &= grid->num_rows - 1;

        selection_start(
            term, start_col, selection_row, SELECTION_CHAR_WISE, false);
    }

    /* Update selection endpoint */
    {
        int selection_row = end_row - grid->view + grid->num_rows;
        selection_row &= grid->num_rows - 1;
        selection_update(term, end_col, selection_row);
    }
}

static ssize_t
matches_cell(const struct terminal *term, const struct cell *cell, size_t search_ofs)
{
    assert(search_ofs < term->search.len);

    char32_t base = cell->wc;
    const struct composed *composed = NULL;

    if (base >= CELL_COMB_CHARS_LO && base <= CELL_COMB_CHARS_HI)
    {
        composed = composed_lookup(term->composed, base - CELL_COMB_CHARS_LO);
        base = composed->chars[0];
    }

    if (composed == NULL && base == 0 && term->search.buf[search_ofs] == U' ')
        return 1;

    if (c32ncasecmp(&base, &term->search.buf[search_ofs], 1) != 0)
        return -1;

    if (composed != NULL) {
        if (search_ofs + 1 + composed->count > term->search.len)
            return -1;

        for (size_t j = 1; j < composed->count; j++) {
            if (composed->chars[j] != term->search.buf[search_ofs + 1 + j])
                return -1;
        }
    }

    return composed != NULL ? 1 + composed->count : 1;
}

static bool
find_next(struct terminal *term, enum search_direction direction,
          struct coord abs_start, struct coord abs_end, struct range *match)
{
#define ROW_DEC(_r) ((_r) = ((_r) - 1 + grid->num_rows) & (grid->num_rows - 1))
#define ROW_INC(_r) ((_r) = ((_r) + 1) & (grid->num_rows - 1))

    struct grid *grid = term->grid;
    const bool backward = direction != SEARCH_FORWARD;

    LOG_DBG("%s: start: %dx%d, end: %dx%d", backward ? "backward" : "forward",
            abs_start.row, abs_start.col, abs_end.row, abs_end.col);

    xassert(abs_start.row >= 0);
    xassert(abs_start.row < grid->num_rows);
    xassert(abs_start.col >= 0);
    xassert(abs_start.col < term->cols);

    xassert(abs_end.row >= 0);
    xassert(abs_end.row < grid->num_rows);
    xassert(abs_end.col >= 0);
    xassert(abs_end.col < term->cols);

    for (int match_start_row = abs_start.row, match_start_col = abs_start.col;
         ;
         backward ? ROW_DEC(match_start_row) : ROW_INC(match_start_row)) {

        const struct row *row = grid->rows[match_start_row];
        if (row == NULL) {
            if (match_start_row == abs_end.row)
                break;
            continue;
        }

        for (;
             backward ? match_start_col >= 0 : match_start_col < term->cols;
             backward ? match_start_col-- : match_start_col++)
        {
            if (matches_cell(term, &row->cells[match_start_col], 0) < 0) {
                if (match_start_row == abs_end.row &&
                    match_start_col == abs_end.col)
                {
                    break;
                }
                continue;
            }

            /*
             * Got a match on the first letter. Now we'll see if the
             * rest of the search buffer matches.
             */

            LOG_DBG("search: initial match at row=%d, col=%d",
                    match_start_row, match_start_col);

            int match_end_row = match_start_row;
            int match_end_col = match_start_col;
            const struct row *match_row = row;
            size_t match_len = 0;

            for (size_t i = 0; i < term->search.len;) {
                if (match_end_col >= term->cols) {
                    ROW_INC(match_end_row);
                    match_end_col = 0;

                    match_row = grid->rows[match_end_row];
                    if (match_row == NULL)
                        break;
                }

                if (match_row->cells[match_end_col].wc >= CELL_SPACER) {
                    match_end_col++;
                    continue;
                }

                ssize_t additional_chars = matches_cell(
                    term, &match_row->cells[match_end_col], i);
                if (additional_chars < 0)
                    break;

                i += additional_chars;
                match_len += additional_chars;
                match_end_col++;
            }

            if (match_len != term->search.len) {
                /* Didn't match (completely) */

                if (match_start_row == abs_end.row &&
                    match_start_col == abs_end.col)
                {
                    break;
                }

                continue;
            }

            *match = (struct range){
                .start = {match_start_col, match_start_row},
                .end = {match_end_col - 1, match_end_row},
            };

            return true;
        }

        if (match_start_row == abs_end.row && match_start_col == abs_end.col)
            break;

        match_start_col = backward ? term->cols - 1 : 0;
    }

    return false;
}

static void
search_find_next(struct terminal *term, enum search_direction direction)
{
    struct grid *grid = term->grid;

    if (term->search.len == 0) {
        term->search.match = (struct coord){-1, -1};
        term->search.match_len = 0;
        selection_cancel(term);
        return;
    }

    struct coord start = term->search.match;
    size_t len = term->search.match_len;

    xassert((len == 0 && start.row == -1 && start.col == -1) ||
           (len > 0 && start.row >= 0 && start.col >= 0));

    if (len == 0) {
        /* No previous match, start from the top, or bottom, of the scrollback */
        switch (direction) {
        case SEARCH_FORWARD:
            start.row = grid_row_absolute_in_view(grid, 0);
            start.col = 0;
            break;

        case SEARCH_BACKWARD:
        case SEARCH_BACKWARD_SAME_POSITION:
            start.row = grid_row_absolute_in_view(grid, term->rows - 1);
            start.col = term->cols - 1;
            break;
        }
    } else {
        /* Continue from last match */
        xassert(start.row >= 0);
        xassert(start.col >= 0);

        switch (direction) {
        case SEARCH_BACKWARD_SAME_POSITION:
            break;

        case SEARCH_BACKWARD:
            if (--start.col < 0) {
                start.col = term->cols - 1;
                start.row += grid->num_rows - 1;
                start.row &= grid->num_rows - 1;
            }
            break;

        case SEARCH_FORWARD:
            if (++start.col >= term->cols) {
                start.col = 0;
                start.row++;
                start.row &= grid->num_rows - 1;
            }
            break;
        }

        xassert(start.row >= 0);
        xassert(start.row < grid->num_rows);
        xassert(start.col >= 0);
        xassert(start.col < term->cols);
    }

    LOG_DBG(
        "update: %s: starting at row=%d col=%d "
        "(offset = %d, view = %d)",
        direction != SEARCH_FORWARD ? "backward" : "forward",
        start.row, start.col,
        grid->offset, grid->view);

    struct coord end = start;
    switch (direction) {
    case SEARCH_FORWARD:
        /* Search forward, until we reach the cell *before* current start */
        if (--end.col < 0) {
            end.col = term->cols - 1;
            end.row += grid->num_rows - 1;
            end.row &= grid->num_rows - 1;
        }
        break;

    case SEARCH_BACKWARD:
    case SEARCH_BACKWARD_SAME_POSITION:
        /* Search backwards, until we reach the cell *after* current start */
        if (++end.col >= term->cols) {
            end.col = 0;
            end.row++;
            end.row &= grid->num_rows - 1;
        }
        break;
    }

    struct range match;
    bool found = find_next(term, direction, start, end, &match);

    if (found) {
        LOG_DBG("primary match found at %dx%d",
                match.start.row, match.start.col);
        search_update_selection(term, &match);
        term->search.match = match.start;
        term->search.match_len = term->search.len;
    } else {
        LOG_DBG("no match");
        term->search.match = (struct coord){-1, -1};
        term->search.match_len = 0;
        selection_cancel(term);
    }
#undef ROW_DEC
}

struct search_match_iterator
search_matches_new_iter(struct terminal *term)
{
    return (struct search_match_iterator){
        .term = term,
        .start = {0, 0},
    };
}

struct range
search_matches_next(struct search_match_iterator *iter)
{
    struct terminal *term = iter->term;
    struct grid *grid = term->grid;

    if (term->search.match_len == 0)
        goto no_match;

    if (iter->start.row >= term->rows)
        goto no_match;

    xassert(iter->start.row >= 0);
    xassert(iter->start.row < term->rows);
    xassert(iter->start.col >= 0);
    xassert(iter->start.col < term->cols);

    struct coord abs_start = iter->start;
    abs_start.row = grid_row_absolute_in_view(grid, abs_start.row);

    struct coord abs_end = {
        term->cols - 1,
        grid_row_absolute_in_view(grid, term->rows - 1)};

    struct range match;
    bool found = find_next(term, SEARCH_FORWARD, abs_start, abs_end, &match);
    if (!found)
        goto no_match;

    LOG_DBG("match at (absolute coordinates) %dx%d-%dx%d",
            match.start.row, match.start.col,
            match.end.row, match.end.col);

    /* Convert absolute row numbers back to view relative */
    match.start.row = match.start.row - grid->view + grid->num_rows;
    match.start.row &= grid->num_rows - 1;
    match.end.row = match.end.row - grid->view + grid->num_rows;
    match.end.row &= grid->num_rows - 1;

    LOG_DBG("match at (view-local coordinates) %dx%d-%dx%d, view=%d",
            match.start.row, match.start.col,
            match.end.row, match.end.col, grid->view);

    xassert(match.start.row >= 0);
    xassert(match.start.row < term->rows);
    xassert(match.end.row >= 0);
    xassert(match.end.row < term->rows);

    /* Assert match end comes *after* the match start */
    xassert(match.end.row > match.start.row ||
            (match.end.row == match.start.row &&
             match.end.col >= match.start.col));

    /* Assert the match starts at, or after, the iterator position */
    xassert(match.start.row > iter->start.row ||
            (match.start.row == iter->start.row &&
             match.start.col >= iter->start.col));

    /* Continue at next column, next time */
    iter->start.row = match.start.row;
    iter->start.col = match.start.col + 1;

    if (iter->start.col >= term->cols) {
        iter->start.col = 0;
        iter->start.row++;  /* Overflow is caught in next iteration */
    }

    xassert(iter->start.row >= 0);
    xassert(iter->start.row <= term->rows);
    xassert(iter->start.col >= 0);
    xassert(iter->start.col < term->cols);
    return match;

no_match:
    iter->start.row = -1;
    iter->start.col = -1;
    return (struct range){{-1, -1}, {-1,  -1}};
}

static void
add_wchars(struct terminal *term, char32_t *src, size_t count)
{
    /* Strip non-printable characters */
    for (size_t i = 0, j = 0, orig_count = count; i < orig_count; i++) {
        if (isc32print(src[i]))
            src[j++] = src[i];
        else
            count--;
    }

    if (!search_ensure_size(term, term->search.len + count))
        return;

    xassert(term->search.len + count < term->search.sz);

    memmove(&term->search.buf[term->search.cursor + count],
            &term->search.buf[term->search.cursor],
            (term->search.len - term->search.cursor) * sizeof(char32_t));

    memcpy(&term->search.buf[term->search.cursor], src, count * sizeof(char32_t));

    term->search.len += count;
    term->search.cursor += count;
    term->search.buf[term->search.len] = U'\0';
}

void
search_add_chars(struct terminal *term, const char *src, size_t count)
{
    size_t chars = mbsntoc32(NULL, src, count, 0);
    if (chars == (size_t)-1) {
        LOG_ERRNO("failed to convert %.*s to Unicode", (int)count, src);
        return;
    }

    char32_t c32s[chars + 1];
    mbsntoc32(c32s, src, count, chars);
    add_wchars(term, c32s, chars);
}

static void
search_match_to_end_of_word(struct terminal *term, bool spaces_only)
{
    if (term->search.match_len == 0)
        return;

    xassert(term->selection.coords.end.row >= 0);

    struct grid *grid = term->grid;
    const bool move_cursor = term->search.cursor == term->search.len;

    struct coord old_end = selection_get_end(term);
    struct coord new_end = old_end;
    struct row *row = NULL;

    xassert(new_end.row >= 0);
    xassert(new_end.row < grid->num_rows);

    /* Advances a coordinate by one column, to the right. Returns
     * false if we’ve reached the scrollback wrap-around */
#define advance_pos(coord) __extension__                                \
        ({                                                              \
            bool wrapped_around = false;                                \
            if (++(coord).col >= term->cols) {                          \
                (coord).row = ((coord).row + 1) & (grid->num_rows - 1); \
                (coord).col = 0;                                        \
                row = grid->rows[(coord).row];                          \
                if (has_wrapped_around(term, (coord.row)))              \
                    wrapped_around = true;                              \
            }                                                           \
            !wrapped_around;                                             \
        })

    /* First character to consider is the *next* character */
    if (!advance_pos(new_end))
        return;

    xassert(new_end.row >= 0);
    xassert(new_end.row < grid->num_rows);
    xassert(grid->rows[new_end.row] != NULL);

    /* Find next word boundary */
    new_end.row -= grid->view + grid->num_rows;
    new_end.row &= grid->num_rows - 1;
    selection_find_word_boundary_right(term, &new_end, spaces_only, false);
    new_end.row += grid->view;
    new_end.row &= grid->num_rows - 1;

    struct coord pos = old_end;
    row = grid->rows[pos.row];

    struct extraction_context *ctx = extract_begin(SELECTION_NONE, false);
    if (ctx == NULL)
        return;

    do {
        if (!advance_pos(pos))
            break;
        if (!extract_one(term, row, &row->cells[pos.col], pos.col, ctx))
            break;
    } while (pos.col != new_end.col || pos.row != new_end.row);

    char32_t *new_text;
    size_t new_len;

    if (!extract_finish_wide(ctx, &new_text, &new_len))
        return;

    if (!search_ensure_size(term, term->search.len + new_len))
        return;

    for (size_t i = 0; i < new_len; i++) {
        if (new_text[i] == U'\n') {
            /* extract() adds newlines, which we never match against */
            continue;
        }

        term->search.buf[term->search.len++] = new_text[i];
    }

    term->search.buf[term->search.len] = U'\0';
    free(new_text);

    if (move_cursor)
        term->search.cursor = term->search.len;

    struct range match = {.start = term->search.match, .end = new_end};
    search_update_selection(term, &match);

    term->search.match_len = term->search.len;

#undef advance_pos
}

static size_t
distance_next_word(const struct terminal *term)
{
    size_t cursor = term->search.cursor;

    /* First eat non-whitespace. This is the word we're skipping past */
    while (cursor < term->search.len) {
        if (isc32space(term->search.buf[cursor++]))
            break;
    }

    xassert(cursor == term->search.len || isc32space(term->search.buf[cursor - 1]));

    /* Now skip past whitespace, so that we end up at the beginning of
     * the next word */
    while (cursor < term->search.len) {
        if (!isc32space(term->search.buf[cursor++]))
            break;
    }

    xassert(cursor == term->search.len || !isc32space(term->search.buf[cursor - 1]));

    if (cursor < term->search.len && !isc32space(term->search.buf[cursor]))
        cursor--;

    return cursor - term->search.cursor;
}

static size_t
distance_prev_word(const struct terminal *term)
{
    int cursor = term->search.cursor;

    /* First, eat whitespace prefix */
    while (cursor > 0) {
        if (!isc32space(term->search.buf[--cursor]))
            break;
    }

    xassert(cursor == 0 || !isc32space(term->search.buf[cursor]));

    /* Now eat non-whitespace. This is the word we're skipping past */
    while (cursor > 0) {
        if (isc32space(term->search.buf[--cursor]))
            break;
    }

    xassert(cursor == 0 || isc32space(term->search.buf[cursor]));
    if (cursor > 0 && isc32space(term->search.buf[cursor]))
        cursor++;

    return term->search.cursor - cursor;
}

static void
from_clipboard_cb(char *text, size_t size, void *user)
{
    struct terminal *term = user;
    search_add_chars(term, text, size);
}

static void
from_clipboard_done(void *user)
{
    struct terminal *term = user;

    LOG_DBG("search: buffer: %ls", (const wchar_t *)term->search.buf);
    search_find_next(term, SEARCH_BACKWARD_SAME_POSITION);
    render_refresh_search(term);
}

static bool
execute_binding(struct seat *seat, struct terminal *term,
                const struct key_binding *binding, uint32_t serial,
                bool *update_search_result, enum search_direction *direction,
                bool *redraw)
{
    *update_search_result = *redraw = false;
    const enum bind_action_search action = binding->action;

    struct grid *grid = term->grid;

    switch (action) {
    case BIND_ACTION_SEARCH_NONE:
        return false;

    case BIND_ACTION_SEARCH_CANCEL:
        if (term->search.view_followed_offset)
            grid->view = grid->offset;
        else {
            grid->view = ensure_view_is_allocated(
                term, term->search.original_view);
        }
        search_cancel(term);
        return true;

    case BIND_ACTION_SEARCH_COMMIT:
        selection_finalize(seat, term, serial);
        search_cancel_keep_selection(term);
        return true;

    case BIND_ACTION_SEARCH_FIND_PREV:
        if (term->search.last.buf != NULL && term->search.len == 0) {
            add_wchars(term, term->search.last.buf, term->search.last.len);

            free(term->search.last.buf);
            term->search.last.buf = NULL;
            term->search.last.len = 0;
        }

        *direction = SEARCH_BACKWARD;
        *update_search_result = *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_FIND_NEXT:
        if (term->search.last.buf != NULL && term->search.len == 0) {
            add_wchars(term, term->search.last.buf, term->search.last.len);

            free(term->search.last.buf);
            term->search.last.buf = NULL;
            term->search.last.len = 0;
        }

        *direction = SEARCH_FORWARD;
        *update_search_result = *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_EDIT_LEFT:
        if (term->search.cursor > 0) {
            term->search.cursor--;
            *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_EDIT_LEFT_WORD: {
        size_t diff = distance_prev_word(term);
        term->search.cursor -= diff;
        xassert(term->search.cursor <= term->search.len);

        if (diff > 0)
            *redraw = true;
        return true;
    }

    case BIND_ACTION_SEARCH_EDIT_RIGHT:
        if (term->search.cursor < term->search.len) {
            term->search.cursor++;
            *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_EDIT_RIGHT_WORD: {
        size_t diff = distance_next_word(term);
        term->search.cursor += diff;
        xassert(term->search.cursor <= term->search.len);

        if (diff > 0)
            *redraw = true;
        return true;
    }

    case BIND_ACTION_SEARCH_EDIT_HOME:
        if (term->search.cursor != 0) {
            term->search.cursor = 0;
            *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_EDIT_END:
        if (term->search.cursor != term->search.len) {
            term->search.cursor = term->search.len;
            *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_DELETE_PREV:
        if (term->search.cursor > 0) {
            memmove(
                &term->search.buf[term->search.cursor - 1],
                &term->search.buf[term->search.cursor],
                (term->search.len - term->search.cursor) * sizeof(char32_t));
            term->search.cursor--;
            term->search.buf[--term->search.len] = U'\0';
            *update_search_result = *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_DELETE_PREV_WORD: {
        size_t diff = distance_prev_word(term);
        size_t old_cursor = term->search.cursor;
        size_t new_cursor = old_cursor - diff;

        if (diff > 0) {
            memmove(&term->search.buf[new_cursor],
                    &term->search.buf[old_cursor],
                    (term->search.len - old_cursor) * sizeof(char32_t));

            term->search.len -= diff;
            term->search.cursor = new_cursor;
            *update_search_result = *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_DELETE_NEXT:
        if (term->search.cursor < term->search.len) {
            memmove(
                &term->search.buf[term->search.cursor],
                &term->search.buf[term->search.cursor + 1],
                (term->search.len - term->search.cursor - 1) * sizeof(char32_t));
            term->search.buf[--term->search.len] = U'\0';
            *update_search_result = *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_DELETE_NEXT_WORD: {
        size_t diff = distance_next_word(term);
        size_t cursor = term->search.cursor;

        if (diff > 0) {
            memmove(&term->search.buf[cursor],
                    &term->search.buf[cursor + diff],
                    (term->search.len - (cursor + diff)) * sizeof(char32_t));

            term->search.len -= diff;
            *update_search_result = *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_EXTEND_WORD:
        search_match_to_end_of_word(term, false);
        *update_search_result = false;
        *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_EXTEND_WORD_WS:
        search_match_to_end_of_word(term, true);
        *update_search_result = false;
        *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_CLIPBOARD_PASTE:
        text_from_clipboard(
            seat, term, &from_clipboard_cb, &from_clipboard_done, term);
        *update_search_result = *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_PRIMARY_PASTE:
        text_from_primary(
            seat, term, &from_clipboard_cb, &from_clipboard_done, term);
        *update_search_result = *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_COUNT:
        BUG("Invalid action type");
        return true;
    }

    BUG("Unhandled action type");
    return false;
}

void
search_input(struct seat *seat, struct terminal *term,
             const struct key_binding_set *bindings, uint32_t key,
             xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
             xkb_mod_mask_t locked,
             const xkb_keysym_t *raw_syms, size_t raw_count,
             uint32_t serial)
{
    LOG_DBG("search: input: sym=%d/0x%x, mods=0x%08x, consumed=0x%08x",
            sym, sym, mods, consumed);

    const xkb_mod_mask_t bind_mods =
        mods & seat->kbd.bind_significant & ~locked;
    const xkb_mod_mask_t bind_consumed =
        consumed & seat->kbd.bind_significant & ~locked;
    enum xkb_compose_status compose_status = seat->kbd.xkb_compose_state != NULL
      ? xkb_compose_state_get_status(seat->kbd.xkb_compose_state)
      : XKB_COMPOSE_NOTHING;

    enum search_direction search_direction = SEARCH_BACKWARD_SAME_POSITION;
    bool update_search_result = false;
    bool redraw = false;

    /* Key bindings */
    tll_foreach(bindings->search, it) {
        const struct key_binding *bind = &it->item;

        /* Match translated symbol */
        if (bind->k.sym == sym &&
            bind->mods == (bind_mods & ~bind_consumed)) {

            if (execute_binding(seat, term, bind, serial,
                                &update_search_result, &search_direction,
                                &redraw))
            {
                goto update_search;
            }
            return;
        }

        if (bind->mods != bind_mods || bind_mods != (mods & ~locked))
            continue;

        /* Match untranslated symbols */
        for (size_t i = 0; i < raw_count; i++) {
            if (bind->k.sym == raw_syms[i]) {
                if (execute_binding(seat, term, bind, serial,
                                    &update_search_result, &search_direction,
                                    &redraw))
                {
                    goto update_search;
                }
                return;
            }
        }

        /* Match raw key code */
        tll_foreach(bind->k.key_codes, code) {
            if (code->item == key) {
                if (execute_binding(seat, term, bind, serial,
                                    &update_search_result, &search_direction,
                                    &redraw))
                {
                    goto update_search;
                }
                return;
            }
        }
    }

    uint8_t buf[64] = {0};
    int count = 0;

    if (compose_status == XKB_COMPOSE_COMPOSED) {
        count = xkb_compose_state_get_utf8(
            seat->kbd.xkb_compose_state, (char *)buf, sizeof(buf));
        xkb_compose_state_reset(seat->kbd.xkb_compose_state);
    } else if (compose_status == XKB_COMPOSE_CANCELLED) {
        count = 0;
    } else {
        count = xkb_state_key_get_utf8(
            seat->kbd.xkb_state, key, (char *)buf, sizeof(buf));
    }

    update_search_result = redraw = count > 0;
    search_direction = SEARCH_BACKWARD_SAME_POSITION;

    if (count == 0)
        return;

    search_add_chars(term, (const char *)buf, count);

update_search:
    LOG_DBG("search: buffer: %ls", (const wchar_t *)term->search.buf);
    if (update_search_result)
        search_find_next(term, search_direction);
    if (redraw)
        render_refresh_search(term);
}
