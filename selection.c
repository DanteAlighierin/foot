#include "selection.h"

#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <pixman.h>

#define LOG_MODULE "selection"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "async.h"
#include "char32.h"
#include "commands.h"
#include "config.h"
#include "extract.h"
#include "grid.h"
#include "misc.h"
#include "render.h"
#include "search.h"
#include "uri.h"
#include "util.h"
#include "vt.h"
#include "xmalloc.h"

static const char *const mime_type_map[] = {
    [DATA_OFFER_MIME_UNSET] = NULL,
    [DATA_OFFER_MIME_TEXT_PLAIN] = "text/plain",
    [DATA_OFFER_MIME_TEXT_UTF8] = "text/plain;charset=utf-8",
    [DATA_OFFER_MIME_URI_LIST] = "text/uri-list",

    [DATA_OFFER_MIME_TEXT_TEXT] = "TEXT",
    [DATA_OFFER_MIME_TEXT_STRING] = "STRING",
    [DATA_OFFER_MIME_TEXT_UTF8_STRING] = "UTF8_STRING",
};

static inline struct coord
bounded(const struct grid *grid, struct coord coord)
{
    coord.row &= grid->num_rows - 1;
    return coord;
}

struct coord
selection_get_start(const struct terminal *term)
{
    if (term->selection.coords.start.row < 0)
        return term->selection.coords.start;
    return bounded(term->grid, term->selection.coords.start);
}

struct coord
selection_get_end(const struct terminal *term)
{
    if (term->selection.coords.end.row < 0)
        return term->selection.coords.end;
    return bounded(term->grid, term->selection.coords.end);
}

bool
selection_on_rows(const struct terminal *term, int row_start, int row_end)
{
    xassert(term->selection.coords.end.row >= 0);

    LOG_DBG("on rows: %d-%d, range: %d-%d (offset=%d)",
            term->selection.coords.start.row, term->selection.coords.end.row,
            row_start, row_end, term->grid->offset);

    row_start += term->grid->offset;
    row_end += term->grid->offset;
    xassert(row_end >= row_start);

    const struct coord *start = &term->selection.coords.start;
    const struct coord *end = &term->selection.coords.end;

    const struct grid *grid = term->grid;
    const int sb_start = grid->offset + term->rows;

    /* Use scrollback relative coords when checking for overlap */
    const int rel_row_start =
        grid_row_abs_to_sb_precalc_sb_start(grid, sb_start, row_start);
    const int rel_row_end =
        grid_row_abs_to_sb_precalc_sb_start(grid, sb_start, row_end);
    int rel_sel_start =
        grid_row_abs_to_sb_precalc_sb_start(grid, sb_start, start->row);
    int rel_sel_end =
        grid_row_abs_to_sb_precalc_sb_start(grid, sb_start, end->row);

    if (rel_sel_start > rel_sel_end) {
        int tmp = rel_sel_start;
        rel_sel_start = rel_sel_end;
        rel_sel_end = tmp;
    }

    if ((rel_row_start <= rel_sel_start && rel_row_end >= rel_sel_start) ||
        (rel_row_start <= rel_sel_end && rel_row_end >= rel_sel_end))
    {
        /* The range crosses one of the selection boundaries */
        return true;
    }

    if (rel_row_start >= rel_sel_start && rel_row_end <= rel_sel_end)
        return true;

    return false;
}

void
selection_scroll_up(struct terminal *term, int rows)
{
    xassert(term->selection.coords.end.row >= 0);

    const int rel_row_start =
        grid_row_abs_to_sb(term->grid, term->rows, term->selection.coords.start.row);
    const int rel_row_end =
        grid_row_abs_to_sb(term->grid, term->rows, term->selection.coords.end.row);
    const int actual_start = min(rel_row_start, rel_row_end);

    if (actual_start - rows < 0) {
        /* Part of the selection will be scrolled out, cancel it */
        selection_cancel(term);
    }
}

void
selection_scroll_down(struct terminal *term, int rows)
{
    xassert(term->selection.coords.end.row >= 0);

    const struct grid *grid = term->grid;
    const struct range *sel = &term->selection.coords;

    const int screen_end =
        grid_row_abs_to_sb(grid, term->rows, grid->offset + term->rows - 1);
    const int rel_row_start =
        grid_row_abs_to_sb(term->grid, term->rows, sel->start.row);
    const int rel_row_end =
        grid_row_abs_to_sb(term->grid, term->rows, sel->end.row);
    const int actual_end = max(rel_row_start, rel_row_end);

    if (actual_end > screen_end - rows) {
        /* Part of the selection will be scrolled out, cancel it */
        selection_cancel(term);
    }
}

void
selection_view_up(struct terminal *term, int new_view)
{
    if (likely(term->selection.coords.start.row < 0))
        return;

    if (likely(new_view < term->grid->view))
        return;

    term->selection.coords.start.row += term->grid->num_rows;
    if (term->selection.coords.end.row >= 0)
        term->selection.coords.end.row += term->grid->num_rows;
}

void
selection_view_down(struct terminal *term, int new_view)
{
    if (likely(term->selection.coords.start.row < 0))
        return;

    if (likely(new_view > term->grid->view))
        return;

    term->selection.coords.start.row &= term->grid->num_rows - 1;
    if (term->selection.coords.end.row >= 0)
        term->selection.coords.end.row &= term->grid->num_rows - 1;
}

static void
foreach_selected_normal(
    struct terminal *term, struct coord _start, struct coord _end,
    bool (*cb)(struct terminal *term, struct row *row, struct cell *cell,
               int row_no, int col, void *data),
    void *data)
{
    const struct coord *start = &_start;
    const struct coord *end = &_end;

    const int grid_rows = term->grid->num_rows;

    /* Start/end rows, relative to the scrollback start */
    /* Start/end rows, relative to the scrollback start */
    const int rel_start_row =
        grid_row_abs_to_sb(term->grid, term->rows, start->row);
    const int rel_end_row =
        grid_row_abs_to_sb(term->grid, term->rows, end->row);

    int start_row, end_row;
    int start_col, end_col;

    if (rel_start_row < rel_end_row) {
        start_row = start->row;
        start_col = start->col;
        end_row = end->row;
        end_col = end->col;
    } else if (rel_start_row > rel_end_row) {
        start_row = end->row;
        start_col = end->col;
        end_row = start->row;
        end_col = start->col;
    } else {
        start_row = end_row = start->row;
        start_col = min(start->col, end->col);
        end_col = max(start->col, end->col);
    }

    start_row &= (grid_rows - 1);
    end_row &= (grid_rows - 1);

    for (int r = start_row; r != end_row; r = (r + 1) & (grid_rows - 1)) {
        struct row *row = term->grid->rows[r];
        xassert(row != NULL);

        for (int c = start_col; c <= term->cols - 1; c++) {
            if (!cb(term, row, &row->cells[c], r, c, data))
                return;
        }

        start_col = 0;
    }

    /* Last, partial row */
    struct row *row = term->grid->rows[end_row];
    xassert(row != NULL);

    for (int c = start_col; c <= end_col; c++) {
        if (!cb(term, row, &row->cells[c], end_row, c, data))
            return;
    }
}

static void
foreach_selected_block(
    struct terminal *term, struct coord _start, struct coord _end,
    bool (*cb)(struct terminal *term, struct row *row, struct cell *cell,
               int row_no, int col, void *data),
    void *data)
{
    const struct coord *start = &_start;
    const struct coord *end = &_end;

    const int grid_rows = term->grid->num_rows;

    /* Start/end rows, relative to the scrollback start */
    const int rel_start_row =
        grid_row_abs_to_sb(term->grid, term->rows, start->row);
    const int rel_end_row =
        grid_row_abs_to_sb(term->grid, term->rows, end->row);

    struct coord top_left = {
        .row = (rel_start_row < rel_end_row
                ? start->row : end->row) & (grid_rows - 1),
        .col = min(start->col, end->col),
    };

    struct coord bottom_right = {
        .row = (rel_start_row > rel_end_row
                ? start->row : end->row) & (grid_rows - 1),
        .col = max(start->col, end->col),
    };

    int r = top_left.row;
    while (true) {
        struct row *row = term->grid->rows[r];
        xassert(row != NULL);

        for (int c = top_left.col; c <= bottom_right.col; c++) {
            if (!cb(term, row, &row->cells[c], r, c, data))
                return;
        }

        if (r == bottom_right.row)
            break;

        r++;
        r &= grid_rows - 1;
    }
}

static void
foreach_selected(
    struct terminal *term, struct coord start, struct coord end,
    bool (*cb)(struct terminal *term, struct row *row, struct cell *cell, int row_no, int col, void *data),
    void *data)
{
    switch (term->selection.kind) {
    case SELECTION_CHAR_WISE:
    case SELECTION_WORD_WISE:
    case SELECTION_QUOTE_WISE:
    case SELECTION_LINE_WISE:
        foreach_selected_normal(term, start, end, cb, data);
        return;

    case SELECTION_BLOCK:
        foreach_selected_block(term, start, end, cb, data);
        return;

    case SELECTION_NONE:
        break;
    }

    BUG("Invalid selection kind");
}

static bool
extract_one_const_wrapper(struct terminal *term,
                          struct row *row, struct cell *cell,
                          int row_no, int col, void *data)
{
    return extract_one(term, row, cell, col, data);
}

char *
selection_to_text(const struct terminal *term)
{
    if (term->selection.coords.end.row == -1)
        return NULL;

    struct extraction_context *ctx = extract_begin(term->selection.kind, true);
    if (ctx == NULL)
        return NULL;

    foreach_selected(
        (struct terminal *)term, term->selection.coords.start, term->selection.coords.end,
        &extract_one_const_wrapper, ctx);

    char *text;
    return extract_finish(ctx, &text, NULL) ? text : NULL;
}

/* Coordinates are in *absolute* row numbers (NOT view local) */
void
selection_find_word_boundary_left(const struct terminal *term, struct coord *pos,
                                  bool spaces_only)
{
    const struct grid *grid = term->grid;

    xassert(pos->col >= 0);
    xassert(pos->col < term->cols);
    xassert(pos->row >= 0);
    pos->row &= grid->num_rows - 1;

    const struct row *r = grid->rows[pos->row];
    char32_t c = r->cells[pos->col].wc;

    while (c >= CELL_SPACER) {
        xassert(pos->col > 0);
        if (pos->col == 0)
            return;
        pos->col--;
        c = r->cells[pos->col].wc;
    }

    if (c >= CELL_COMB_CHARS_LO && c <= CELL_COMB_CHARS_HI)
        c = composed_lookup(term->composed, c - CELL_COMB_CHARS_LO)->chars[0];

    bool initial_is_space = c == 0 || isc32space(c);
    bool initial_is_delim =
        !initial_is_space && !isword(c, spaces_only, term->conf->word_delimiters);
    bool initial_is_word =
        c != 0 && isword(c, spaces_only, term->conf->word_delimiters);

    while (true) {
        int next_col = pos->col - 1;
        int next_row = pos->row;

        const struct row *row = grid->rows[next_row];

        /* Linewrap */
        if (next_col < 0) {
            next_col = term->cols - 1;

            next_row = (next_row - 1 + grid->num_rows) & (grid->num_rows - 1);

            if (grid_row_abs_to_sb(grid, term->rows, next_row) == term->grid->num_rows - 1 ||
                grid->rows[next_row] == NULL)
            {
                /* Scrollback wrap-around */
                break;
            }

            row = grid->rows[next_row];

            if (row->linebreak) {
                /* Hard linebreak, treat as space. I.e. break selection */
                break;
            }
        }

        c = row->cells[next_col].wc;
        while (c >= CELL_SPACER) {
            xassert(next_col > 0);
            if (--next_col < 0)
                return;
            c = row->cells[next_col].wc;
        }

        if (c >= CELL_COMB_CHARS_LO && c <= CELL_COMB_CHARS_HI)
            c = composed_lookup(term->composed, c - CELL_COMB_CHARS_LO)->chars[0];

        bool is_space = c == 0 || isc32space(c);
        bool is_delim =
            !is_space && !isword(c, spaces_only, term->conf->word_delimiters);
        bool is_word =
            c != 0 && isword(c, spaces_only, term->conf->word_delimiters);

        if (initial_is_space && !is_space)
            break;
        if (initial_is_delim && !is_delim)
            break;
        if (initial_is_word && !is_word)
            break;

        pos->col = next_col;
        pos->row = next_row;
    }
}

/* Coordinates are in *absolute* row numbers (NOT view local) */
void
selection_find_word_boundary_right(const struct terminal *term, struct coord *pos,
                                   bool spaces_only,
                                   bool stop_on_space_to_word_boundary)
{
    const struct grid *grid = term->grid;

    xassert(pos->col >= 0);
    xassert(pos->col < term->cols);
    xassert(pos->row >= 0);
    pos->row &= grid->num_rows - 1;

    const struct row *r = grid->rows[pos->row];
    char32_t c = r->cells[pos->col].wc;

    while (c >= CELL_SPACER) {
        xassert(pos->col > 0);
        if (pos->col == 0)
            return;
        pos->col--;
        c = r->cells[pos->col].wc;
    }

    if (c >= CELL_COMB_CHARS_LO && c <= CELL_COMB_CHARS_HI)
        c = composed_lookup(term->composed, c - CELL_COMB_CHARS_LO)->chars[0];

    bool initial_is_space = c == 0 || isc32space(c);
    bool initial_is_delim =
        !initial_is_space && !isword(c, spaces_only, term->conf->word_delimiters);
    bool initial_is_word =
        c != 0 && isword(c, spaces_only, term->conf->word_delimiters);
    bool have_seen_word = initial_is_word;

    while (true) {
        int next_col = pos->col + 1;
        int next_row = pos->row;

        const struct row *row = term->grid->rows[next_row];

        /* Linewrap */
        if (next_col >= term->cols) {
            if (row->linebreak) {
                /* Hard linebreak, treat as space. I.e. break selection */
                break;
            }

            next_col = 0;
            next_row = (next_row + 1) & (grid->num_rows - 1);

            if (grid_row_abs_to_sb(grid, term->rows, next_row) == 0) {
                /* Scrollback wrap-around */
                break;
            }

            row = grid->rows[next_row];
        }

        c = row->cells[next_col].wc;
        while (c >= CELL_SPACER) {
            if (++next_col >= term->cols) {
                next_col = 0;
                if (++next_row >= term->rows)
                    return;
            }
            c = row->cells[next_col].wc;
        }

        if (c >= CELL_COMB_CHARS_LO && c <= CELL_COMB_CHARS_HI)
            c = composed_lookup(term->composed, c - CELL_COMB_CHARS_LO)->chars[0];

        bool is_space = c == 0 || isc32space(c);
        bool is_delim =
            !is_space && !isword(c, spaces_only, term->conf->word_delimiters);
        bool is_word =
            c != 0 && isword(c, spaces_only, term->conf->word_delimiters);

        if (stop_on_space_to_word_boundary) {
            if (initial_is_space && !is_space)
                break;
            if (initial_is_delim && !is_delim)
                break;
        } else {
            if (initial_is_space && ((have_seen_word && is_space) || is_delim))
                break;
            if (initial_is_delim && ((have_seen_word && is_delim) || is_space))
                break;
        }
        if (initial_is_word && !is_word)
            break;

        have_seen_word = is_word;

        pos->col = next_col;
        pos->row = next_row;
    }
}

static bool
selection_find_quote_left(struct terminal *term, struct coord *pos,
                          char32_t *quote_char)
{
    const struct row *row = grid_row_in_view(term->grid, pos->row);
    char32_t wc = row->cells[pos->col].wc;

    if ((*quote_char == '\0' && (wc == '"' || wc == '\'')) ||
        wc == *quote_char)
    {
        return false;
    }

    int next_row = pos->row;
    int next_col = pos->col;

    while (true) {
        if (--next_col < 0) {
            next_col = term->cols - 1;
            if (--next_row < 0)
                return false;

            row = grid_row_in_view(term->grid, next_row);
            if (row->linebreak)
                return false;
        }

        wc = row->cells[next_col].wc;

        if ((*quote_char == '\0' && (wc == '"' || wc == '\'')) ||
            wc == *quote_char)
        {
            pos->row = next_row;
            pos->col = next_col + 1;
            xassert(pos->col < term->cols);

            *quote_char = wc;
            return true;
        }
    }
}

static bool
selection_find_quote_right(struct terminal *term, struct coord *pos, char32_t quote_char)
{
    if (quote_char == '\0')
        return false;

    const struct row *row = grid_row_in_view(term->grid, pos->row);
    char32_t wc = row->cells[pos->col].wc;
    if (wc == quote_char)
        return false;

    int next_row = pos->row;
    int next_col = pos->col;

    while (true) {
        if (++next_col >= term->cols) {
            next_col = 0;
            if (++next_row >= term->rows)
                return false;

            if (row->linebreak)
                return false;

            row = grid_row_in_view(term->grid, next_row);
        }

        wc = row->cells[next_col].wc;
        if (wc == quote_char) {
            pos->row = next_row;
            pos->col = next_col - 1;
            xassert(pos->col >= 0);
            return true;
        }
    }
}

static void
selection_find_line_boundary_left(struct terminal *term, struct coord *pos)
{
    int next_row = pos->row;
    pos->col = 0;

    while (true) {
        if (--next_row < 0)
            return;

        const struct row *row = grid_row_in_view(term->grid, next_row);
        assert(row != NULL);

        if (row->linebreak)
            return;

        pos->col = 0;
        pos->row = next_row;
    }
}

static void
selection_find_line_boundary_right(struct terminal *term, struct coord *pos)
{
    int next_row = pos->row;
    pos->col = term->cols - 1;

    while (true) {
        const struct row *row = grid_row_in_view(term->grid, next_row);
        assert(row != NULL);

        if (row->linebreak)
            return;

        if (++next_row >= term->rows)
            return;

        pos->col = term->cols - 1;
        pos->row = next_row;
    }
}

void
selection_start(struct terminal *term, int col, int row,
                enum selection_kind kind,
                bool spaces_only)
{
    selection_cancel(term);

    LOG_DBG("%s selection started at %d,%d",
            kind == SELECTION_CHAR_WISE ? "character-wise" :
            kind == SELECTION_WORD_WISE ? "word-wise" :
            kind == SELECTION_QUOTE_WISE ? "quote-wise" :
            kind == SELECTION_LINE_WISE ? "line-wise" :
            kind == SELECTION_BLOCK ? "block" : "<unknown>",
            row, col);

    term->selection.kind = kind;
    term->selection.ongoing = true;
    term->selection.spaces_only = spaces_only;

    switch (kind) {
    case SELECTION_CHAR_WISE:
    case SELECTION_BLOCK:
        term->selection.coords.start = (struct coord){col, term->grid->view + row};
        term->selection.coords.end = (struct coord){-1, -1};

        term->selection.pivot.start = term->selection.coords.start;
        term->selection.pivot.end = term->selection.coords.end;
        break;

    case SELECTION_WORD_WISE: {
        struct coord start = {col, term->grid->view + row};
        struct coord end = {col, term->grid->view + row};
        selection_find_word_boundary_left(term, &start, spaces_only);
        selection_find_word_boundary_right(term, &end, spaces_only, true);

        term->selection.coords.start = start;

        term->selection.pivot.start = term->selection.coords.start;
        term->selection.pivot.end = end;

        /*
         * FIXME: go through selection.c and make sure all public
         * functions use the *same* coordinate system...
         *
         * selection_find_word_boundary*() uses absolute row numbers,
         * while selection_update(), and pretty much all others, use
         * view-local.
         */

        selection_update(term, end.col, end.row - term->grid->view);
        break;
    }

    case SELECTION_QUOTE_WISE: {
        struct coord start = {col, row}, end = {col, row};

        char32_t quote_char = '\0';
        bool found_left = selection_find_quote_left(term, &start, &quote_char);
        bool found_right = selection_find_quote_right(term, &end, quote_char);

        if (found_left && !found_right) {
            xassert(quote_char != '\0');

            /*
             * Try to flip the quote character we're looking for.
             *
             * This lets us handle things like:
             *
             *   "nested 'quotes are fun', right"
             *
             * In the example above, starting the selection at
             * "right", will otherwise not match. find-left will find
             * the single quote, causing find-right to fail.
             *
             * By flipping the quote-character, and re-trying, we
             * find-left will find the starting double quote, letting
             * find-right succeed as well.
             */

            if (quote_char == '\'')
                quote_char = '"';
            else if (quote_char == '"')
                quote_char = '\'';

            found_left = selection_find_quote_left(term, &start, &quote_char);
            found_right = selection_find_quote_right(term, &end, quote_char);
        }

        if (found_left && found_right) {
            term->selection.coords.start = (struct coord){
                start.col, term->grid->view + start.row};

            term->selection.pivot.start = term->selection.coords.start;
            term->selection.pivot.end = (struct coord){end.col, term->grid->view + end.row};

            term->selection.kind = SELECTION_WORD_WISE;
            selection_update(term, end.col, end.row);
            break;
        } else {
            term->selection.kind = SELECTION_LINE_WISE;
            /* FALLTHROUGH */
        }
    }

    case SELECTION_LINE_WISE: {
        struct coord start = {0, row}, end = {term->cols - 1, row};
        selection_find_line_boundary_left(term, &start);
        selection_find_line_boundary_right(term, &end);

        term->selection.coords.start = (struct coord){
            start.col, term->grid->view + start.row};
        term->selection.pivot.start = term->selection.coords.start;
        term->selection.pivot.end = (struct coord){end.col, term->grid->view + end.row};

        selection_update(term, end.col, end.row);
        break;
    }

    case SELECTION_NONE:
        BUG("Invalid selection kind");
        break;
    }

}

static pixman_region32_t
pixman_region_for_coords_normal(const struct terminal *term,
                                const struct coord *start,
                                const struct coord *end)
{
    pixman_region32_t region;
    pixman_region32_init(&region);

    const int rel_start_row =
        grid_row_abs_to_sb(term->grid, term->rows, start->row);
    const int rel_end_row =
        grid_row_abs_to_sb(term->grid, term->rows, end->row);

    if (rel_start_row < rel_end_row) {
        /* First partial row (start ->)*/
        pixman_region32_union_rect(
            &region, &region,
            start->col, rel_start_row,
            term->cols - start->col, 1);

        /* Full rows between start and end */
        if (rel_start_row + 1 < rel_end_row) {
            pixman_region32_union_rect(
                &region, &region,
                0, rel_start_row + 1,
                term->cols, rel_end_row - rel_start_row - 1);
        }

        /* Last partial row (-> end) */
        pixman_region32_union_rect(
            &region, &region,
            0, rel_end_row,
            end->col + 1, 1);

    } else if (rel_start_row > rel_end_row) {
        /* First partial row (end ->) */
        pixman_region32_union_rect(
            &region, &region,
            end->col, rel_end_row,
            term->cols - end->col, 1);

        /* Full rows between end and start */
        if (rel_end_row + 1 < rel_start_row) {
            pixman_region32_union_rect(
                &region, &region,
                0, rel_end_row + 1,
                term->cols, rel_start_row - rel_end_row - 1);
        }

        /* Last partial row (-> start) */
        pixman_region32_union_rect(
            &region, &region,
            0, rel_start_row,
            start->col + 1, 1);
    } else {
        const int start_col = min(start->col, end->col);
        const int end_col = max(start->col, end->col);

        pixman_region32_union_rect(
            &region, &region,
            start_col, rel_start_row,
            end_col + 1 - start_col, 1);
    }

    return region;
}

static pixman_region32_t
pixman_region_for_coords_block(const struct terminal *term,
                               const struct coord *start, const struct coord *end)
{
    pixman_region32_t region;
    pixman_region32_init(&region);

    const int rel_start_row =
        grid_row_abs_to_sb(term->grid, term->rows, start->row);
    const int rel_end_row =
        grid_row_abs_to_sb(term->grid, term->rows, end->row);

    pixman_region32_union_rect(
        &region, &region,
        min(start->col, end->col), min(rel_start_row, rel_end_row),
        abs(start->col - end->col) + 1, abs(rel_start_row - rel_end_row) + 1);

    return region;
}

/* Returns a pixman region representing the selection between 'start'
 * and 'end' (given the current selection kind), in *scrollback
 * relative coordinates* */
static pixman_region32_t
pixman_region_for_coords(const struct terminal *term,
                         const struct coord *start, const struct coord *end)
{
    switch (term->selection.kind) {
    default:              return pixman_region_for_coords_normal(term, start, end);
    case SELECTION_BLOCK: return pixman_region_for_coords_block(term, start, end);
    }
}

enum mark_selection_variant {
    MARK_SELECTION_MARK_AND_DIRTY,
    MARK_SELECTION_UNMARK_AND_DIRTY,
    MARK_SELECTION_MARK_FOR_RENDER,
};

static void
mark_selected_region(struct terminal *term, pixman_box32_t *boxes,
                     size_t count, enum mark_selection_variant mark_variant)
{
    const bool selected =
        mark_variant == MARK_SELECTION_MARK_AND_DIRTY ||
        mark_variant == MARK_SELECTION_MARK_FOR_RENDER;
    const bool dirty_cells =
        mark_variant == MARK_SELECTION_MARK_AND_DIRTY ||
        mark_variant == MARK_SELECTION_UNMARK_AND_DIRTY;
    const bool highlight_empty =
        mark_variant != MARK_SELECTION_MARK_FOR_RENDER ||
        term->selection.kind == SELECTION_BLOCK;

    for (size_t i = 0; i < count; i++) {
        const pixman_box32_t *box = &boxes[i];

        LOG_DBG("%s selection in region: %dx%d - %dx%d",
                selected ? "marking" : "unmarking",
                box->x1, box->y1,
                box->x2, box->y2);

        int abs_row_start = grid_row_sb_to_abs(
            term->grid, term->rows, box->y1);

        for (int r = abs_row_start, rel_r = box->y1;
             rel_r < box->y2;
             r = (r + 1) & (term->grid->num_rows - 1), rel_r++)
        {
            struct row *row = term->grid->rows[r];
            xassert(row != NULL);

            if (dirty_cells)
                row->dirty = true;

            for (int c = box->x1, empty_count = 0; c < box->x2; c++) {
                struct cell *cell = &row->cells[c];

                if (cell->wc == 0 && !highlight_empty) {
                    /*
                     * We used to highlight empty cells *if* they were
                     * followed by non-empty cell(s), since this
                     * corresponds to what gets extracted when the
                     * selection is copied (that is, empty cells
                     * "between" non-empty cells are converted to
                     * spaces).
                     *
                     * However, they way we handle selection updates
                     * (diffing the "old" selection area against the
                     * "new" one, using pixman regions), means we
                     * can't correctly update the state of empty
                     * cells. The result is "random" empty cells being
                     * rendered as selected when they shouldn't.
                     *
                     * "Fix" by *never* highlighting selected empty
                     * cells (they still get converted to spaces when
                     * copied, if followed by non-empty cells).
                     */
                    empty_count++;

                    /*
                     * When the selection is *modified*, empty cells
                     * are treated just like non-empty cells; they are
                     * marked as selected, and dirtied.
                     *
                     * This is due to how the algorithm for updating
                     * the selection works; it uses regions to
                     * calculate the difference between the "old" and
                     * the "new" selection. This makes it impossible
                     * to tell if an empty cell is a *trailing* empty
                     * cell (that should not be highlighted), or an
                     * empty cells between non-empty cells (that
                     * *should* be highlighted).
                     *
                     * Then, when a frame is rendered, we loop the
                     * *visibible* cells that belong to the
                     * selection. At this point, we *can* tell if an
                     * empty cell is trailing or not.
                     *
                     * So, what we need to do is check if a
                     * 'selected', and empty cell has been marked as
                     * selected, temporarily unmark (forcing it dirty,
                     * to ensure it gets re-rendered). If it is *not*
                     * a trailing empty cell, it will get re-tagged as
                     * selected in the for-loop below.
                     */
                    cell->attrs.clean = false;
                    cell->attrs.selected = false;
                    row->dirty = true;
                    continue;
                }

                for (int j = 0; j < empty_count + 1; j++) {
                    xassert(c - j >= 0);
                    struct cell *cell = &row->cells[c - j];

                    if (dirty_cells) {
                        cell->attrs.clean = false;
                        row->dirty = true;
                    }
                    cell->attrs.selected = selected;
                }

                empty_count = 0;
            }
        }
    }
}

static void
selection_modify(struct terminal *term, struct coord start, struct coord end)
{
    xassert(term->selection.coords.start.row != -1);
    xassert(start.row != -1 && start.col != -1);
    xassert(end.row != -1 && end.col != -1);

    pixman_region32_t previous_selection;
    if (term->selection.coords.end.row >= 0) {
        previous_selection = pixman_region_for_coords(
            term,
            &term->selection.coords.start,
            &term->selection.coords.end);
    } else
        pixman_region32_init(&previous_selection);

    pixman_region32_t current_selection = pixman_region_for_coords(
        term, &start, &end);

    pixman_region32_t no_longer_selected;
    pixman_region32_init(&no_longer_selected);
    pixman_region32_subtract(
        &no_longer_selected, &previous_selection, &current_selection);

    pixman_region32_t newly_selected;
    pixman_region32_init(&newly_selected);
    pixman_region32_subtract(
        &newly_selected, &current_selection, &previous_selection);

    /* Clear selection in cells no longer selected */
    int n_rects = -1;
    pixman_box32_t *boxes = NULL;

    boxes = pixman_region32_rectangles(&no_longer_selected, &n_rects);
    mark_selected_region(term, boxes, n_rects, MARK_SELECTION_UNMARK_AND_DIRTY);

    boxes = pixman_region32_rectangles(&newly_selected, &n_rects);
    mark_selected_region(term, boxes, n_rects, MARK_SELECTION_MARK_AND_DIRTY);

    pixman_region32_fini(&newly_selected);
    pixman_region32_fini(&no_longer_selected);
    pixman_region32_fini(&current_selection);
    pixman_region32_fini(&previous_selection);

    term->selection.coords.start = start;
    term->selection.coords.end = end;
    render_refresh(term);
}

static void
set_pivot_point_for_block_and_char_wise(struct terminal *term,
                                        struct coord start,
                                        enum selection_direction new_direction)
{
    struct coord *pivot_start = &term->selection.pivot.start;
    struct coord *pivot_end = &term->selection.pivot.end;

    *pivot_start = start;

    /* First, make sure 'start' isn't in the middle of a
     * multi-column character */
    while (true) {
        const struct row *row = term->grid->rows[pivot_start->row & (term->grid->num_rows - 1)];
        const struct cell *cell = &row->cells[pivot_start->col];

        if (cell->wc < CELL_SPACER)
            break;

        /* Multi-column chars don't cross rows */
        xassert(pivot_start->col > 0);
        if (pivot_start->col == 0)
            break;

        pivot_start->col--;
    }

    /*
     * Setup pivot end to be one character *before* start
     * Which one we move, the end or start point, depends
     * on the initial selection direction.
     */

    *pivot_end = *pivot_start;

    if (new_direction == SELECTION_RIGHT) {
        bool keep_going = true;
        while (keep_going) {
            const struct row *row = term->grid->rows[pivot_end->row & (term->grid->num_rows - 1)];
            const char32_t wc = row->cells[pivot_end->col].wc;

            keep_going = wc >= CELL_SPACER;

            if (pivot_end->col == 0) {
                if (pivot_end->row - term->grid->view <= 0)
                    break;
                pivot_end->col = term->cols - 1;
                pivot_end->row--;
            } else
                pivot_end->col--;
        }
    } else {
        bool keep_going = true;
        while (keep_going) {
            const struct row *row = term->grid->rows[pivot_start->row & (term->grid->num_rows - 1)];
            const char32_t wc = pivot_start->col < term->cols - 1
                ? row->cells[pivot_start->col + 1].wc : 0;

            keep_going = wc >= CELL_SPACER;

            if (pivot_start->col >= term->cols - 1) {
                if (pivot_start->row - term->grid->view >= term->rows - 1)
                    break;
                pivot_start->col = 0;
                pivot_start->row++;
            } else
                pivot_start->col++;
        }
    }

    xassert(term->grid->rows[pivot_start->row & (term->grid->num_rows - 1)]->
           cells[pivot_start->col].wc <= CELL_SPACER);
    xassert(term->grid->rows[pivot_end->row & (term->grid->num_rows - 1)]->
           cells[pivot_end->col].wc <= CELL_SPACER + 1);
}

void
selection_update(struct terminal *term, int col, int row)
{
    if (term->selection.coords.start.row < 0)
        return;

    if (!term->selection.ongoing)
        return;

    xassert(term->grid->view + row != -1);

    struct coord new_start = term->selection.coords.start;
    struct coord new_end = {col, term->grid->view + row};

    LOG_DBG("selection updated: start = %d,%d, end = %d,%d -> %d, %d",
            term->selection.coords.start.row, term->selection.coords.start.col,
            term->selection.coords.end.row, term->selection.coords.end.col,
            new_end.row, new_end.col);

    /* Adjust start point if the selection has changed 'direction' */
    if (!(new_end.row == new_start.row && new_end.col == new_start.col)) {
        enum selection_direction new_direction = term->selection.direction;

        struct coord *pivot_start = &term->selection.pivot.start;
        struct coord *pivot_end = &term->selection.pivot.end;

        if (term->selection.kind == SELECTION_BLOCK) {
            if (new_end.col > pivot_start->col)
                new_direction = SELECTION_RIGHT;
            else
                new_direction = SELECTION_LEFT;

            if (term->selection.direction == SELECTION_UNDIR)
                set_pivot_point_for_block_and_char_wise(term, *pivot_start, new_direction);

            if (new_direction == SELECTION_LEFT)
                new_start = *pivot_end;
            else
                new_start = *pivot_start;
            term->selection.direction = new_direction;
        } else {
            if (new_end.row < pivot_start->row ||
                (new_end.row == pivot_start->row &&
                 new_end.col < pivot_start->col))
            {
                /* New end point is before the start point */
                new_direction = SELECTION_LEFT;
            } else {
                /* The new end point is after the start point */
                new_direction = SELECTION_RIGHT;
            }

            if (term->selection.direction != new_direction) {
                if (term->selection.direction == SELECTION_UNDIR &&
                    pivot_end->row < 0)
                {
                    set_pivot_point_for_block_and_char_wise(
                        term, *pivot_start, new_direction);
                }

                if (new_direction == SELECTION_LEFT) {
                    xassert(pivot_end->row >= 0);
                    new_start = *pivot_end;
                } else
                    new_start = *pivot_start;

                term->selection.direction = new_direction;
            }
        }
    }

    switch (term->selection.kind) {
    case SELECTION_CHAR_WISE:
    case SELECTION_BLOCK:
        break;

    case SELECTION_WORD_WISE:
        switch (term->selection.direction) {
        case SELECTION_LEFT:
            new_end = (struct coord){col, term->grid->view + row};
            selection_find_word_boundary_left(
                term, &new_end, term->selection.spaces_only);
            break;

        case SELECTION_RIGHT:
            new_end = (struct coord){col, term->grid->view + row};
            selection_find_word_boundary_right(
                term, &new_end, term->selection.spaces_only, true);
            break;

        case SELECTION_UNDIR:
            break;
        }
        break;

    case SELECTION_QUOTE_WISE:
        BUG("quote-wise selection should always be transformed to either word-wise or line-wise");
        break;

    case SELECTION_LINE_WISE:
        switch (term->selection.direction) {
        case SELECTION_LEFT: {
            struct coord end = {0, row};
            selection_find_line_boundary_left(term, &end);
            new_end = (struct coord){end.col, term->grid->view + end.row};
            break;
        }

        case SELECTION_RIGHT: {
            struct coord end = {col, row};
            selection_find_line_boundary_right(term, &end);
            new_end = (struct coord){end.col, term->grid->view + end.row};
            break;
        }

        case SELECTION_UNDIR:
            break;
        }
        break;

    case SELECTION_NONE:
        BUG("Invalid selection kind");
        break;
    }

    size_t start_row_idx = new_start.row & (term->grid->num_rows - 1);
    size_t end_row_idx = new_end.row & (term->grid->num_rows - 1);

    const struct row *row_start = term->grid->rows[start_row_idx];
    const struct row *row_end = term->grid->rows[end_row_idx];

    /* If an end point is in the middle of a multi-column character,
     * expand the selection to cover the entire character */
    if (new_start.row < new_end.row ||
        (new_start.row == new_end.row && new_start.col <= new_end.col))
    {
        while (new_start.col >= 1 &&
               row_start->cells[new_start.col].wc >= CELL_SPACER)
            new_start.col--;
        while (new_end.col < term->cols - 1 &&
               row_end->cells[new_end.col + 1].wc >= CELL_SPACER)
            new_end.col++;
    } else {
        while (new_end.col >= 1 &&
               row_end->cells[new_end.col].wc >= CELL_SPACER)
            new_end.col--;
        while (new_start.col < term->cols - 1 &&
               row_start->cells[new_start.col + 1].wc >= CELL_SPACER)
            new_start.col++;
    }

    selection_modify(term, new_start, new_end);
}

void
selection_dirty_cells(struct terminal *term)
{
    if (term->selection.coords.start.row < 0 || term->selection.coords.end.row < 0)
        return;

    pixman_region32_t selection = pixman_region_for_coords(
        term, &term->selection.coords.start, &term->selection.coords.end);

    pixman_region32_t view = pixman_region_for_coords(
        term,
        &(struct coord){0, term->grid->view},
        &(struct coord){term->cols - 1, term->grid->view + term->rows - 1});

    pixman_region32_t visible_and_selected;
    pixman_region32_init(&visible_and_selected);
    pixman_region32_intersect(&visible_and_selected, &selection, &view);

    int n_rects = -1;
    pixman_box32_t *boxes =
        pixman_region32_rectangles(&visible_and_selected, &n_rects);
    mark_selected_region(term, boxes, n_rects, MARK_SELECTION_MARK_FOR_RENDER);

    pixman_region32_fini(&visible_and_selected);
    pixman_region32_fini(&view);
    pixman_region32_fini(&selection);
}

static void
selection_extend_normal(struct terminal *term, int col, int row,
                        enum selection_kind new_kind)
{
    const struct coord *start = &term->selection.coords.start;
    const struct coord *end = &term->selection.coords.end;

    const int rel_row = grid_row_abs_to_sb(term->grid, term->rows, row);
    int rel_start_row = grid_row_abs_to_sb(term->grid, term->rows, start->row);
    int rel_end_row = grid_row_abs_to_sb(term->grid, term->rows, end->row);

    if (rel_start_row > rel_end_row ||
        (rel_start_row == rel_end_row && start->col > end->col))
    {
        const struct coord *tmp = start;
        start = end;
        end = tmp;

        int tmp_row = rel_start_row;
        rel_start_row = rel_end_row;
        rel_end_row = tmp_row;
    }

    struct coord new_start, new_end;
    enum selection_direction direction;

    if (rel_row < rel_start_row ||
        (rel_row == rel_start_row && col < start->col))
    {
        /* Extend selection to start *before* current start */
        new_start = *end;
        new_end = (struct coord){col, row};
        direction = SELECTION_LEFT;
    }

    else if (rel_row > rel_end_row ||
             (rel_row == rel_end_row && col > end->col))
    {
        /* Extend selection to end *after* current end */
        new_start = *start;
        new_end = (struct coord){col, row};
        direction = SELECTION_RIGHT;
    }

    else {
        /* Shrink selection from start or end, depending on which one is closest */

        const int linear = rel_row * term->cols + col;

        if (abs(linear - (rel_start_row * term->cols + start->col)) <
            abs(linear - (rel_end_row * term->cols + end->col)))
        {
            /* Move start point */
            new_start = *end;
            new_end = (struct coord){col, row};
            direction = SELECTION_LEFT;
        }

        else {
            /* Move end point */
            new_start = *start;
            new_end = (struct coord){col, row};
            direction = SELECTION_RIGHT;
        }
    }

    const bool spaces_only = term->selection.spaces_only;

    switch (term->selection.kind) {
    case SELECTION_CHAR_WISE:
        xassert(new_kind == SELECTION_CHAR_WISE);
        set_pivot_point_for_block_and_char_wise(term, new_start, direction);
        break;

    case SELECTION_WORD_WISE: {
        xassert(new_kind == SELECTION_CHAR_WISE ||
               new_kind == SELECTION_WORD_WISE);

        struct coord pivot_start = {new_start.col, new_start.row};
        struct coord pivot_end = pivot_start;

        selection_find_word_boundary_left(term, &pivot_start, spaces_only);
        selection_find_word_boundary_right(term, &pivot_end, spaces_only, true);

        term->selection.pivot.start = pivot_start;
        term->selection.pivot.end = pivot_end;
        break;
    }

    case SELECTION_QUOTE_WISE: {
        BUG("quote-wise selection should always be transformed to either word-wise or line-wise");
        break;
    }

    case SELECTION_LINE_WISE: {
        xassert(new_kind == SELECTION_CHAR_WISE ||
                new_kind == SELECTION_LINE_WISE);

        struct coord pivot_start = {new_start.col, new_start.row - term->grid->view};
        struct coord pivot_end = pivot_start;

        selection_find_line_boundary_left(term, &pivot_start);
        selection_find_line_boundary_right(term, &pivot_end);

        term->selection.pivot.start =
            (struct coord){pivot_start.col, term->grid->view + pivot_start.row};
        term->selection.pivot.end =
            (struct coord){pivot_end.col, term->grid->view + pivot_end.row};
        break;
    }

    case SELECTION_BLOCK:
    case SELECTION_NONE:
        BUG("Invalid selection kind in this context");
        break;
    }

    term->selection.kind = new_kind;
    term->selection.direction = direction;
    selection_modify(term, new_start, new_end);
}

static void
selection_extend_block(struct terminal *term, int col, int row)
{
    const struct coord *start = &term->selection.coords.start;
    const struct coord *end = &term->selection.coords.end;

    const int rel_start_row =
        grid_row_abs_to_sb(term->grid, term->rows, start->row);
    const int rel_end_row =
        grid_row_abs_to_sb(term->grid, term->rows, end->row);

    struct coord top_left = {
        .row = rel_start_row < rel_end_row ? start->row : end->row,
        .col = min(start->col, end->col),
    };

    struct coord top_right = {
        .row = top_left.row,
        .col = max(start->col, end->col),
    };

    struct coord bottom_left = {
        .row = rel_start_row > rel_end_row ? start->row : end->row,
        .col = min(start->col, end->col),
    };

    struct coord bottom_right = {
        .row = bottom_left.row,
        .col = max(start->col, end->col),
    };

    const int rel_row = grid_row_abs_to_sb(term->grid, term->rows, row);
    const int rel_top_row = grid_row_abs_to_sb(term->grid, term->rows, top_left.row);
    const int rel_bottom_row = grid_row_abs_to_sb(term->grid, term->rows, bottom_left.row);
    struct coord new_start;
    struct coord new_end;

    enum selection_direction direction = SELECTION_UNDIR;

    if (rel_row <= rel_top_row ||
        abs(rel_row - rel_top_row) < abs(rel_row - rel_bottom_row))
    {
        /* Move one of the top corners */

        if (abs(col - top_left.col) < abs(col - top_right.col)) {
            new_start = bottom_right;
            new_end = (struct coord){col, row};
        }

        else {
            new_start = bottom_left;
            new_end = (struct coord){col, row};
        }
    }

    else {
        /* Move one of the bottom corners */

        if (abs(col - bottom_left.col) < abs(col - bottom_right.col)) {
            new_start = top_right;
            new_end = (struct coord){col, row};
        }

        else {
            new_start = top_left;
            new_end = (struct coord){col, row};
        }
    }

    direction = col > new_start.col ? SELECTION_RIGHT : SELECTION_LEFT;
    set_pivot_point_for_block_and_char_wise(term, new_start, direction);

    term->selection.direction = direction;
    selection_modify(term, new_start, new_end);
}

void
selection_extend(struct seat *seat, struct terminal *term,
                 int col, int row, enum selection_kind new_kind)
{
    if (term->selection.coords.start.row < 0 || term->selection.coords.end.row < 0) {
        /* No existing selection */
        return;
    }

    if (term->selection.kind == SELECTION_BLOCK && new_kind != SELECTION_BLOCK)
        return;

    term->selection.ongoing = true;

    row += term->grid->view;

    if ((row == term->selection.coords.start.row && col == term->selection.coords.start.col) ||
        (row == term->selection.coords.end.row && col == term->selection.coords.end.col))
    {
        /* Extension point *is* one of the current end points */
        return;
    }

    switch (term->selection.kind) {
    case SELECTION_NONE:
        BUG("Invalid selection kind");
        return;

    case SELECTION_CHAR_WISE:
    case SELECTION_WORD_WISE:
    case SELECTION_QUOTE_WISE:
    case SELECTION_LINE_WISE:
        selection_extend_normal(term, col, row, new_kind);
        break;

    case SELECTION_BLOCK:
        selection_extend_block(term, col, row);
        break;
    }
}

//static const struct zwp_primary_selection_source_v1_listener primary_selection_source_listener;

void
selection_finalize(struct seat *seat, struct terminal *term, uint32_t serial)
{
    if (!term->selection.ongoing)
        return;

    LOG_DBG("selection finalize");

    selection_stop_scroll_timer(term);
    term->selection.ongoing = false;

    if (term->selection.coords.start.row < 0 || term->selection.coords.end.row < 0)
        return;

    xassert(term->selection.coords.start.row != -1);
    xassert(term->selection.coords.end.row != -1);

    term->selection.coords.start.row &= (term->grid->num_rows - 1);
    term->selection.coords.end.row &= (term->grid->num_rows - 1);

    switch (term->conf->selection_target) {
    case SELECTION_TARGET_NONE:
        break;

    case SELECTION_TARGET_PRIMARY:
        selection_to_primary(seat, term, serial);
        break;
    case SELECTION_TARGET_CLIPBOARD:
        selection_to_clipboard(seat, term, serial);
        break;

    case SELECTION_TARGET_BOTH:
        selection_to_primary(seat, term, serial);
        selection_to_clipboard(seat, term, serial);
        break;
    }
}

static bool
unmark_selected(struct terminal *term, struct row *row, struct cell *cell,
                int row_no, int col, void *data)
{
    if (!cell->attrs.selected)
        return true;

    row->dirty = true;
    cell->attrs.selected = false;
    cell->attrs.clean = false;
    return true;
}

void
selection_cancel(struct terminal *term)
{
    LOG_DBG("selection cancelled: start = %d,%d end = %d,%d",
            term->selection.coords.start.row, term->selection.coords.start.col,
            term->selection.coords.end.row, term->selection.coords.end.col);

    selection_stop_scroll_timer(term);

    if (term->selection.coords.start.row >= 0 && term->selection.coords.end.row >= 0) {
        foreach_selected(
            term, term->selection.coords.start, term->selection.coords.end,
            &unmark_selected, NULL);
        render_refresh(term);
    }

    term->selection.kind = SELECTION_NONE;
    term->selection.coords.start = (struct coord){-1, -1};
    term->selection.coords.end = (struct coord){-1, -1};
    term->selection.pivot.start = (struct coord){-1, -1};
    term->selection.pivot.end = (struct coord){-1, -1};
    term->selection.direction = SELECTION_UNDIR;
    term->selection.ongoing = false;

    search_selection_cancelled(term);
}

bool
selection_clipboard_has_data(const struct seat *seat)
{
    return seat->clipboard.data_offer != NULL;
}

bool
selection_primary_has_data(const struct seat *seat)
{
    return seat->primary.data_offer != NULL;
}

void
selection_clipboard_unset(struct seat *seat)
{
    struct wl_clipboard *clipboard = &seat->clipboard;

    if (clipboard->data_source == NULL)
        return;

    /* Kill previous data source */
    xassert(clipboard->serial != 0);
    wl_data_device_set_selection(seat->data_device, NULL, clipboard->serial);
    wl_data_source_destroy(clipboard->data_source);

    clipboard->data_source = NULL;
    clipboard->serial = 0;

    free(clipboard->text);
    clipboard->text = NULL;
}

void
selection_primary_unset(struct seat *seat)
{
    struct wl_primary *primary = &seat->primary;

    if (primary->data_source == NULL)
        return;

    xassert(primary->serial != 0);
    zwp_primary_selection_device_v1_set_selection(
        seat->primary_selection_device, NULL, primary->serial);
    zwp_primary_selection_source_v1_destroy(primary->data_source);

    primary->data_source = NULL;
    primary->serial = 0;

    free(primary->text);
    primary->text = NULL;
}

static bool
fdm_scroll_timer(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct terminal *term = data;

    uint64_t expiration_count;
    ssize_t ret = read(
        term->selection.auto_scroll.fd,
        &expiration_count, sizeof(expiration_count));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read selection scroll timer");
        return false;
    }

    switch (term->selection.auto_scroll.direction) {
    case SELECTION_SCROLL_NOT:
        return true;

    case SELECTION_SCROLL_UP:
        cmd_scrollback_up(term, expiration_count);
        selection_update(term, term->selection.auto_scroll.col, 0);
        break;

    case SELECTION_SCROLL_DOWN:
        cmd_scrollback_down(term, expiration_count);
        selection_update(term, term->selection.auto_scroll.col, term->rows - 1);
        break;
    }


    return true;
}

void
selection_start_scroll_timer(struct terminal *term, int interval_ns,
                             enum selection_scroll_direction direction, int col)
{
    xassert(direction != SELECTION_SCROLL_NOT);

    if (!term->selection.ongoing)
        return;

    if (term->selection.auto_scroll.fd < 0) {
        int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (fd < 0) {
            LOG_ERRNO("failed to create selection scroll timer");
            goto err;
        }

        if (!fdm_add(term->fdm, fd, EPOLLIN, &fdm_scroll_timer, term)) {
            close(fd);
            return;
        }

        term->selection.auto_scroll.fd = fd;
    }

    struct itimerspec timer;
    if (timerfd_gettime(term->selection.auto_scroll.fd, &timer) < 0) {
        LOG_ERRNO("failed to get current selection scroll timer value");
        goto err;
    }

    if (timer.it_value.tv_sec == 0 && timer.it_value.tv_nsec == 0)
        timer.it_value.tv_nsec = 1;

    timer.it_interval.tv_sec = interval_ns / 1000000000;
    timer.it_interval.tv_nsec = interval_ns % 1000000000;

    if (timerfd_settime(term->selection.auto_scroll.fd, 0, &timer, NULL) < 0) {
        LOG_ERRNO("failed to set new selection scroll timer value");
        goto err;
    }

    term->selection.auto_scroll.direction = direction;
    term->selection.auto_scroll.col = col;
    return;

err:
    selection_stop_scroll_timer(term);
    return;
}

void
selection_stop_scroll_timer(struct terminal *term)
{
    if (term->selection.auto_scroll.fd < 0) {
        xassert(term->selection.auto_scroll.direction == SELECTION_SCROLL_NOT);
        return;
    }

    fdm_del(term->fdm, term->selection.auto_scroll.fd);
    term->selection.auto_scroll.fd = -1;
    term->selection.auto_scroll.direction = SELECTION_SCROLL_NOT;
}

static void
target(void *data, struct wl_data_source *wl_data_source, const char *mime_type)
{
    LOG_DBG("TARGET: mime-type=%s", mime_type);
}

struct clipboard_send {
    char *data;
    size_t len;
    size_t idx;
};

static bool
fdm_send(struct fdm *fdm, int fd, int events, void *data)
{
    struct clipboard_send *ctx = data;

    if (events & EPOLLHUP)
        goto done;

    switch (async_write(fd, ctx->data, ctx->len, &ctx->idx)) {
    case ASYNC_WRITE_REMAIN:
        return true;

    case ASYNC_WRITE_DONE:
        break;

    case ASYNC_WRITE_ERR:
        LOG_ERRNO(
            "failed to asynchronously write %zu of selection data to FD=%d",
            ctx->len - ctx->idx, fd);
        break;
    }

done:
    fdm_del(fdm, fd);
    free(ctx->data);
    free(ctx);
    return true;
}

static void
send_clipboard_or_primary(struct seat *seat, int fd, const char *selection,
                          const char *source_name)
{
    /* Make it NONBLOCK:ing right away - we don't want to block if the
     * initial attempt to send the data synchronously fails */
    int flags;
    if ((flags = fcntl(fd, F_GETFL)) < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        LOG_ERRNO("failed to set O_NONBLOCK");
        return;
    }

    size_t len = selection != NULL ? strlen(selection) : 0;
    size_t async_idx = 0;

    switch (async_write(fd, selection, len, &async_idx)) {
    case ASYNC_WRITE_REMAIN: {
        struct clipboard_send *ctx = xmalloc(sizeof(*ctx));
        *ctx = (struct clipboard_send) {
            .data = xstrdup(&selection[async_idx]),
            .len = len - async_idx,
            .idx = 0,
        };

        if (fdm_add(seat->wayl->fdm, fd, EPOLLOUT, &fdm_send, ctx))
            return;

        free(ctx->data);
        free(ctx);
        break;
    }

    case ASYNC_WRITE_DONE:
        break;

    case ASYNC_WRITE_ERR:
        LOG_ERRNO("failed write %zu bytes of %s selection data to FD=%d",
                  len, source_name, fd);
        break;
    }

    close(fd);
}

static void
send(void *data, struct wl_data_source *wl_data_source, const char *mime_type,
     int32_t fd)
{
    struct seat *seat = data;
    const struct wl_clipboard *clipboard = &seat->clipboard;

    send_clipboard_or_primary(seat, fd, clipboard->text, "clipboard");
}

static void
cancelled(void *data, struct wl_data_source *wl_data_source)
{
    struct seat *seat = data;
    struct wl_clipboard *clipboard = &seat->clipboard;
    xassert(clipboard->data_source == wl_data_source);

    wl_data_source_destroy(clipboard->data_source);
    clipboard->data_source = NULL;
    clipboard->serial = 0;

    free(clipboard->text);
    clipboard->text = NULL;
}

/* We don't support dragging *from* */
static void
dnd_drop_performed(void *data, struct wl_data_source *wl_data_source)
{
    //LOG_DBG("DnD drop performed");
}

static void
dnd_finished(void *data, struct wl_data_source *wl_data_source)
{
    //LOG_DBG("DnD finished");
}

static void
action(void *data, struct wl_data_source *wl_data_source, uint32_t dnd_action)
{
    //LOG_DBG("DnD action: %u", dnd_action);
}

static const struct wl_data_source_listener data_source_listener = {
    .target = &target,
    .send = &send,
    .cancelled = &cancelled,
    .dnd_drop_performed = &dnd_drop_performed,
    .dnd_finished = &dnd_finished,
    .action = &action,
};

static void
primary_send(void *data,
             struct zwp_primary_selection_source_v1 *zwp_primary_selection_source_v1,
             const char *mime_type, int32_t fd)
{
    struct seat *seat = data;
    const struct wl_primary *primary = &seat->primary;

    send_clipboard_or_primary(seat, fd, primary->text, "primary");
}

static void
primary_cancelled(void *data,
                  struct zwp_primary_selection_source_v1 *zwp_primary_selection_source_v1)
{
    struct seat *seat = data;
    struct wl_primary *primary = &seat->primary;

    zwp_primary_selection_source_v1_destroy(primary->data_source);
    primary->data_source = NULL;
    primary->serial = 0;

    free(primary->text);
    primary->text = NULL;
}

static const struct zwp_primary_selection_source_v1_listener primary_selection_source_listener = {
    .send = &primary_send,
    .cancelled = &primary_cancelled,
};

bool
text_to_clipboard(struct seat *seat, struct terminal *term, char *text, uint32_t serial)
{
    xassert(serial != 0);

    struct wl_clipboard *clipboard = &seat->clipboard;

    if (clipboard->data_source != NULL) {
        /* Kill previous data source */
        xassert(clipboard->serial != 0);
        wl_data_device_set_selection(seat->data_device, NULL, clipboard->serial);
        wl_data_source_destroy(clipboard->data_source);
        free(clipboard->text);

        clipboard->data_source = NULL;
        clipboard->serial = 0;
        clipboard->text = NULL;
    }

    clipboard->data_source
        = wl_data_device_manager_create_data_source(term->wl->data_device_manager);

    if (clipboard->data_source == NULL) {
        LOG_ERR("failed to create clipboard data source");
        return false;
    }

    clipboard->text = text;

    /* Configure source */
    wl_data_source_offer(clipboard->data_source, mime_type_map[DATA_OFFER_MIME_TEXT_UTF8]);
    wl_data_source_offer(clipboard->data_source, mime_type_map[DATA_OFFER_MIME_TEXT_PLAIN]);
    wl_data_source_offer(clipboard->data_source, mime_type_map[DATA_OFFER_MIME_TEXT_TEXT]);;
    wl_data_source_offer(clipboard->data_source, mime_type_map[DATA_OFFER_MIME_TEXT_STRING]);
    wl_data_source_offer(clipboard->data_source, mime_type_map[DATA_OFFER_MIME_TEXT_UTF8_STRING]);

    wl_data_source_add_listener(clipboard->data_source, &data_source_listener, seat);
    wl_data_device_set_selection(seat->data_device, clipboard->data_source, serial);

    /* Needed when sending the selection to other client */
    clipboard->serial = serial;
    return true;
}

void
selection_to_clipboard(struct seat *seat, struct terminal *term, uint32_t serial)
{
    if (term->selection.coords.start.row < 0 || term->selection.coords.end.row < 0)
        return;

    /* Get selection as a string */
    char *text = selection_to_text(term);
    if (!text_to_clipboard(seat, term, text, serial))
        free(text);
}

struct clipboard_receive {
    int read_fd;
    int timeout_fd;
    struct itimerspec timeout;
    bool bracketed;
    bool quote_paths;

    void (*decoder)(struct clipboard_receive *ctx, char *data, size_t size);
    void (*finish)(struct clipboard_receive *ctx);

    /* URI state */
    bool add_space;
    struct {
        char *data;
        size_t sz;
        size_t idx;
    } buf;

    /* Callback data */
    void (*cb)(char *data, size_t size, void *user);
    void (*done)(void *user);
    void *user;
};

static void
clipboard_receive_done(struct fdm *fdm, struct clipboard_receive *ctx)
{
    fdm_del(fdm, ctx->timeout_fd);
    fdm_del(fdm, ctx->read_fd);
    ctx->done(ctx->user);
    free(ctx->buf.data);
    free(ctx);
}

static bool
fdm_receive_timeout(struct fdm *fdm, int fd, int events, void *data)
{
    struct clipboard_receive *ctx = data;
    if (events & EPOLLHUP)
        return false;

    xassert(events & EPOLLIN);

    uint64_t expire_count;
    ssize_t ret = read(fd, &expire_count, sizeof(expire_count));
    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read clipboard timeout timer");
        return false;
    }

    LOG_WARN("no data received from clipboard in %llu seconds, aborting",
             (unsigned long long)ctx->timeout.it_value.tv_sec);

    clipboard_receive_done(fdm, ctx);
    return true;
}

static void
fdm_receive_decoder_plain(struct clipboard_receive *ctx, char *data, size_t size)
{
    ctx->cb(data, size, ctx->user);
}

static void
fdm_receive_finish_plain(struct clipboard_receive *ctx)
{
}

static bool
decode_one_uri(struct clipboard_receive *ctx, char *uri, size_t len)
{
    LOG_DBG("URI: \"%.*s\"", (int)len, uri);

    if (len == 0)
        return false;

    char *scheme, *host, *path;
    if (!uri_parse(uri, len, &scheme, NULL, NULL, &host, NULL, &path, NULL, NULL)) {
        LOG_ERR("drag-and-drop: invalid URI: %.*s", (int)len, uri);
        return false;
    }

    if (ctx->add_space)
        ctx->cb(" ", 1, ctx->user);
    ctx->add_space = true;

    if (streq(scheme, "file") && hostname_is_localhost(host)) {
        if (ctx->quote_paths)
            ctx->cb("'", 1, ctx->user);

        ctx->cb(path, strlen(path), ctx->user);

        if (ctx->quote_paths)
            ctx->cb("'", 1, ctx->user);
    } else
        ctx->cb(uri, len, ctx->user);

    free(scheme);
    free(host);
    free(path);
    return true;
}

static void
fdm_receive_decoder_uri(struct clipboard_receive *ctx, char *data, size_t size)
{
    while (ctx->buf.idx + size > ctx->buf.sz) {
        size_t new_sz = ctx->buf.sz == 0 ? size : 2 * ctx->buf.sz;
        ctx->buf.data = xrealloc(ctx->buf.data, new_sz);
        ctx->buf.sz = new_sz;
    }

    memcpy(&ctx->buf.data[ctx->buf.idx], data, size);
    ctx->buf.idx += size;

    char *start = ctx->buf.data;
    char *end = NULL;

    while (true) {
        for (end = start; end < &ctx->buf.data[ctx->buf.idx]; end++) {
            if (*end == '\r' || *end == '\n')
                break;
        }

        if (end >= &ctx->buf.data[ctx->buf.idx])
            break;

        decode_one_uri(ctx, start, end - start);
        start = end + 1;
    }

    const size_t ofs = start - ctx->buf.data;
    const size_t left = ctx->buf.idx - ofs;

    memmove(&ctx->buf.data[0], &ctx->buf.data[ofs], left);
    ctx->buf.idx = left;
}

static void
fdm_receive_finish_uri(struct clipboard_receive *ctx)
{
    LOG_DBG("finish: %.*s", (int)ctx->buf.idx, ctx->buf.data);
    decode_one_uri(ctx, ctx->buf.data, ctx->buf.idx);
}

static bool
fdm_receive(struct fdm *fdm, int fd, int events, void *data)
{
    struct clipboard_receive *ctx = data;

    if ((events & EPOLLHUP) && !(events & EPOLLIN))
        goto done;

    /* Reset timeout timer */
    if (timerfd_settime(ctx->timeout_fd, 0, &ctx->timeout, NULL) < 0) {
        LOG_ERRNO("failed to re-arm clipboard timeout timer");
        return false;
    }

    /* Read until EOF */
    while (true) {
        char text[256];
        ssize_t count = read(fd, text, sizeof(text));

        if (count == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;

            LOG_ERRNO("failed to read clipboard data");
            break;
        }

        if (count == 0)
            break;

        /*
         * Call cb while at same time replace:
         *   - \r\n -> \r  (non-bracketed paste)
         *   - \n -> \r    (non-bracketed paste)
         *   - C0 -> <nothing>  (strip non-formatting C0 characters)
         *   - \e -> <nothing>  (i.e. strip ESC)
         */
        char *p = text;
        size_t left = count;

#define skip_one()                              \
        do {                                    \
            ctx->decoder(ctx, p, i);            \
            xassert(i + 1 <= left);             \
            p += i + 1;                         \
            left -= i + 1;                      \
        } while (0)

    again:
        for (size_t i = 0; i < left; i++) {
            switch (p[i]) {
            default:
                break;

            case '\n':
                if (!ctx->bracketed)
                    p[i] = '\r';
                break;

            case '\r':
                /* Convert \r\n -> \r */
                if (!ctx->bracketed && i + 1 < left && p[i + 1] == '\n') {
                    i++;
                    skip_one();
                    goto again;
                }
                break;

            /* C0 non-formatting control characters (\b \t \n \r excluded) */
            case '\x01': case '\x02': case '\x03': case '\x04': case '\x05':
            case '\x06': case '\x07': case '\x0e': case '\x0f': case '\x10':
            case '\x11': case '\x12': case '\x13': case '\x14': case '\x15':
            case '\x16': case '\x17': case '\x18': case '\x19': case '\x1a':
            case '\x1b': case '\x1c': case '\x1d': case '\x1e': case '\x1f':
                skip_one();
                goto again;

            /*
             * In addition to stripping non-formatting C0 controls,
             * XTerm has an option, "disallowedPasteControls", that
             * defines C0 controls that will be replaced with spaces
             * when pasted.
             *
             * It's default value is BS,DEL,ENQ,EOT,NUL
             *
             * Instead of replacing them with spaces, we allow them in
             * bracketed paste mode, and strip them completely in
             * non-bracketed mode.
             *
             * Note some of the (default) XTerm controls are already
             * handled above.
             */
            case '\b': case '\x7f': case '\x00':
                if (!ctx->bracketed) {
                    skip_one();
                    goto again;
                }
                break;
            }
        }

        ctx->decoder(ctx, p, left);
        left = 0;
    }

#undef skip_one

done:
    ctx->finish(ctx);
    clipboard_receive_done(fdm, ctx);
    return true;
}

static void
begin_receive_clipboard(struct terminal *term, int read_fd,
                        enum data_offer_mime_type mime_type,
                        void (*cb)(char *data, size_t size, void *user),
                        void (*done)(void *user), void *user)
{
    int timeout_fd = -1;
    struct clipboard_receive *ctx = NULL;

    int flags;
    if ((flags = fcntl(read_fd, F_GETFL)) < 0 ||
        fcntl(read_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        LOG_ERRNO("failed to set O_NONBLOCK");
        goto err;
    }

    timeout_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timeout_fd < 0) {
        LOG_ERRNO("failed to create clipboard timeout timer FD");
        goto err;
    }

    const struct itimerspec timeout = {.it_value = {.tv_sec = 2}};
    if (timerfd_settime(timeout_fd, 0, &timeout, NULL) < 0) {
        LOG_ERRNO("failed to arm clipboard timeout timer");
        goto err;
    }

    ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct clipboard_receive) {
        .read_fd = read_fd,
        .timeout_fd = timeout_fd,
        .timeout = timeout,
        .bracketed = term->bracketed_paste,
        .quote_paths = term->grid == &term->normal,
        .decoder = (mime_type == DATA_OFFER_MIME_URI_LIST
                    ? &fdm_receive_decoder_uri
                    : &fdm_receive_decoder_plain),
        .finish = (mime_type == DATA_OFFER_MIME_URI_LIST
                   ? &fdm_receive_finish_uri
                   : &fdm_receive_finish_plain),
        .cb = cb,
        .done = done,
        .user = user,
    };

    if (!fdm_add(term->fdm, read_fd, EPOLLIN, &fdm_receive, ctx) ||
        !fdm_add(term->fdm, timeout_fd, EPOLLIN, &fdm_receive_timeout, ctx))
    {
        goto err;
    }

    return;

err:
    free(ctx);
    fdm_del(term->fdm, timeout_fd);
    fdm_del(term->fdm, read_fd);
    done(user);
}

void
text_from_clipboard(struct seat *seat, struct terminal *term,
                    void (*cb)(char *data, size_t size, void *user),
                    void (*done)(void *user), void *user)
{
    struct wl_clipboard *clipboard = &seat->clipboard;
    if (clipboard->data_offer == NULL ||
        clipboard->mime_type == DATA_OFFER_MIME_UNSET)
    {
        done(user);
        return;
    }

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        done(user);
        return;
    }

    LOG_DBG("receive from clipboard: mime-type=%s",
            mime_type_map[clipboard->mime_type]);

    int read_fd = fds[0];
    int write_fd = fds[1];

    /* Give write-end of pipe to other client */
    wl_data_offer_receive(
        clipboard->data_offer, mime_type_map[clipboard->mime_type], write_fd);

    /* Don't keep our copy of the write-end open (or we'll never get EOF) */
    close(write_fd);

    begin_receive_clipboard(term, read_fd, clipboard->mime_type, cb, done, user);
}

static void
receive_offer(char *data, size_t size, void *user)
{
    struct terminal *term = user;
    xassert(term->is_sending_paste_data);
    term_paste_data_to_slave(term, data, size);
}

static void
receive_offer_done(void *user)
{
    struct terminal *term = user;

    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[201~", 6);

    term->is_sending_paste_data = false;

    /* Make sure we send any queued up non-paste data */
    if (tll_length(term->ptmx_buffers) > 0)
        fdm_event_add(term->fdm, term->ptmx, EPOLLOUT);
}

void
selection_from_clipboard(struct seat *seat, struct terminal *term, uint32_t serial)
{
    if (term->is_sending_paste_data) {
        /* We're already pasting... */
        return;
    }

    struct wl_clipboard *clipboard = &seat->clipboard;
    if (clipboard->data_offer == NULL)
        return;

    term->is_sending_paste_data = true;

    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[200~", 6);

    text_from_clipboard(seat, term, &receive_offer, &receive_offer_done, term);
}

bool
text_to_primary(struct seat *seat, struct terminal *term, char *text, uint32_t serial)
{
    if (term->wl->primary_selection_device_manager == NULL)
        return false;

    xassert(serial != 0);

    struct wl_primary *primary = &seat->primary;

    /* TODO: somehow share code with the clipboard equivalent */
    if (seat->primary.data_source != NULL) {
        /* Kill previous data source */

        xassert(primary->serial != 0);
        zwp_primary_selection_device_v1_set_selection(
            seat->primary_selection_device, NULL, primary->serial);
        zwp_primary_selection_source_v1_destroy(primary->data_source);
        free(primary->text);

        primary->data_source = NULL;
        primary->serial = 0;
        primary->text = NULL;
    }

    primary->data_source
        = zwp_primary_selection_device_manager_v1_create_source(
            term->wl->primary_selection_device_manager);

    if (primary->data_source == NULL) {
        LOG_ERR("failed to create clipboard data source");
        return false;
    }

    /* Get selection as a string */
    primary->text = text;

    /* Configure source */
    zwp_primary_selection_source_v1_offer(primary->data_source, mime_type_map[DATA_OFFER_MIME_TEXT_UTF8]);
    zwp_primary_selection_source_v1_offer(primary->data_source, mime_type_map[DATA_OFFER_MIME_TEXT_PLAIN]);
    zwp_primary_selection_source_v1_offer(primary->data_source, mime_type_map[DATA_OFFER_MIME_TEXT_TEXT]);
    zwp_primary_selection_source_v1_offer(primary->data_source, mime_type_map[DATA_OFFER_MIME_TEXT_STRING]);
    zwp_primary_selection_source_v1_offer(primary->data_source, mime_type_map[DATA_OFFER_MIME_TEXT_UTF8_STRING]);

    zwp_primary_selection_source_v1_add_listener(primary->data_source, &primary_selection_source_listener, seat);
    zwp_primary_selection_device_v1_set_selection(seat->primary_selection_device, primary->data_source, serial);

    /* Needed when sending the selection to other client */
    primary->serial = serial;
    return true;
}

void
selection_to_primary(struct seat *seat, struct terminal *term, uint32_t serial)
{
    if (term->wl->primary_selection_device_manager == NULL)
        return;

    /* Get selection as a string */
    char *text = selection_to_text(term);
    if (!text_to_primary(seat, term, text, serial))
        free(text);
}

void
text_from_primary(
    struct seat *seat, struct terminal *term,
    void (*cb)(char *data, size_t size, void *user),
    void (*done)(void *user), void *user)
{
    if (term->wl->primary_selection_device_manager == NULL) {
        done(user);
        return;
    }

    struct wl_primary *primary = &seat->primary;
    if (primary->data_offer == NULL ||
        primary->mime_type == DATA_OFFER_MIME_UNSET)
    {
        done(user);
        return;
    }

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        done(user);
        return;
    }

    LOG_DBG("receive from primary: mime-type=%s",
            mime_type_map[primary->mime_type]);

    int read_fd = fds[0];
    int write_fd = fds[1];

    /* Give write-end of pipe to other client */
    zwp_primary_selection_offer_v1_receive(
        primary->data_offer, mime_type_map[primary->mime_type], write_fd);

    /* Don't keep our copy of the write-end open (or we'll never get EOF) */
    close(write_fd);

    begin_receive_clipboard(term, read_fd, primary->mime_type, cb, done, user);
}

void
selection_from_primary(struct seat *seat, struct terminal *term)
{
    if (term->wl->primary_selection_device_manager == NULL)
        return;

    if (term->is_sending_paste_data) {
        /* We're already pasting... */
        return;
    }

    struct wl_primary *primary = &seat->primary;
    if (primary->data_offer == NULL)
        return;

    term->is_sending_paste_data = true;
    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[200~", 6);

    text_from_primary(seat, term, &receive_offer, &receive_offer_done, term);
}

static void
select_mime_type_for_offer(const char *_mime_type,
                           enum data_offer_mime_type *type)
{
    enum data_offer_mime_type mime_type = DATA_OFFER_MIME_UNSET;

    /* Translate offered mime type to our mime type enum */
    for (size_t i = 0; i < ALEN(mime_type_map); i++) {
        if (mime_type_map[i] == NULL)
            continue;

        if (streq(_mime_type, mime_type_map[i])) {
            mime_type = i;
            break;
        }
    }

    LOG_DBG("mime-type: %s -> %s (offered type was %s)",
            mime_type_map[*type], mime_type_map[mime_type], _mime_type);

    /* Mime-type transition; if the new mime-type is "better" than
     * previously offered types, use the new type */

    switch (mime_type) {
    case DATA_OFFER_MIME_TEXT_PLAIN:
    case DATA_OFFER_MIME_TEXT_TEXT:
    case DATA_OFFER_MIME_TEXT_STRING:
        /* text/plain is our least preferred type. Only use if current
         * type is unset */
        switch (*type) {
        case DATA_OFFER_MIME_UNSET:
            *type = mime_type;
            break;

        default:
            break;
        }
        break;

    case DATA_OFFER_MIME_TEXT_UTF8:
    case DATA_OFFER_MIME_TEXT_UTF8_STRING:
        /* text/plain;charset=utf-8 is preferred over text/plain */
        switch (*type) {
        case DATA_OFFER_MIME_UNSET:
        case DATA_OFFER_MIME_TEXT_PLAIN:
        case DATA_OFFER_MIME_TEXT_TEXT:
        case DATA_OFFER_MIME_TEXT_STRING:
            *type = mime_type;
            break;

        default:
            break;
        }
        break;

    case DATA_OFFER_MIME_URI_LIST:
        /* text/uri-list is always used when offered */
        *type = mime_type;
        break;

    case DATA_OFFER_MIME_UNSET:
        break;
    }
}

static void
data_offer_reset(struct wl_clipboard *clipboard)
{
    if (clipboard->data_offer != NULL) {
        wl_data_offer_destroy(clipboard->data_offer);
        clipboard->data_offer = NULL;
    }

    clipboard->window = NULL;
    clipboard->mime_type = DATA_OFFER_MIME_UNSET;
}

static void
offer(void *data, struct wl_data_offer *wl_data_offer, const char *mime_type)
{
    struct seat *seat = data;
    select_mime_type_for_offer(mime_type, &seat->clipboard.mime_type);
}

static void
source_actions(void *data, struct wl_data_offer *wl_data_offer,
                uint32_t source_actions)
{
#if defined(_DEBUG) && LOG_ENABLE_DBG
    char actions_as_string[1024];
    size_t idx = 0;

    actions_as_string[0] = '\0';
    actions_as_string[sizeof(actions_as_string) - 1] = '\0';

    for (size_t i = 0; i < 31; i++) {
        if (((source_actions >> i) & 1) == 0)
            continue;

        enum wl_data_device_manager_dnd_action action = 1 << i;

        const char *s = NULL;

        switch (action) {
        case WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE: s = NULL; break;
        case WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY: s = "copy"; break;
        case WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE: s = "move"; break;
        case WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK: s = "ask"; break;
        }

        if (s == NULL)
            continue;

        strncat(actions_as_string, s, sizeof(actions_as_string) - idx - 1);
        idx += strlen(s);
        strncat(actions_as_string, ", ", sizeof(actions_as_string) - idx - 1);
        idx += 2;
    }

    /* Strip trailing ", " */
    if (strlen(actions_as_string) > 2)
        actions_as_string[strlen(actions_as_string) - 2] = '\0';

    LOG_DBG("DnD actions: %s (0x%08x)", actions_as_string, source_actions);
#endif
}

static void
offer_action(void *data, struct wl_data_offer *wl_data_offer, uint32_t dnd_action)
{
#if defined(_DEBUG) && LOG_ENABLE_DBG
    const char *s = NULL;

    switch (dnd_action) {
    case WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE: s = "<none>"; break;
    case WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY: s = "copy"; break;
    case WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE: s = "move"; break;
    case WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK: s = "ask"; break;
    }

    LOG_DBG("DnD offer action: %s (0x%08x)", s, dnd_action);
#endif
}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer = &offer,
    .source_actions = &source_actions,
    .action = &offer_action,
};

static void
data_offer(void *data, struct wl_data_device *wl_data_device,
           struct wl_data_offer *offer)
{
    struct seat *seat = data;
    data_offer_reset(&seat->clipboard);
    seat->clipboard.data_offer = offer;
    wl_data_offer_add_listener(offer, &data_offer_listener, seat);
}

static void
enter(void *data, struct wl_data_device *wl_data_device, uint32_t serial,
      struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
      struct wl_data_offer *offer)
{
    struct seat *seat = data;
    struct wayland *wayl = seat->wayl;

    xassert(offer == seat->clipboard.data_offer);

    if (seat->clipboard.mime_type == DATA_OFFER_MIME_UNSET)
        goto reject_offer;

    /* Remember _which_ terminal the current DnD offer is targeting */
    xassert(seat->clipboard.window == NULL);
    tll_foreach(wayl->terms, it) {
        if (term_surface_kind(it->item, surface) == TERM_SURF_GRID &&
            !it->item->is_sending_paste_data)
        {
            wl_data_offer_accept(
                offer, serial, mime_type_map[seat->clipboard.mime_type]);
            wl_data_offer_set_actions(
                offer,
                WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);

            seat->clipboard.window = it->item->window;
            return;
        }
    }

reject_offer:
    /* Either terminal is already busy sending paste data, or mouse
     * pointer isn't over the grid */
    seat->clipboard.window = NULL;
    wl_data_offer_accept(offer, serial, NULL);
    wl_data_offer_set_actions(
        offer,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
}

static void
leave(void *data, struct wl_data_device *wl_data_device)
{
    struct seat *seat = data;
    seat->clipboard.window = NULL;
}

static void
motion(void *data, struct wl_data_device *wl_data_device, uint32_t time,
       wl_fixed_t x, wl_fixed_t y)
{
}

struct dnd_context {
    struct terminal *term;
    struct wl_data_offer *data_offer;
};

static void
receive_dnd(char *data, size_t size, void *user)
{
    struct dnd_context *ctx = user;
    receive_offer(data, size, ctx->term);
}

static void
receive_dnd_done(void *user)
{
    struct dnd_context *ctx = user;

    wl_data_offer_finish(ctx->data_offer);
    wl_data_offer_destroy(ctx->data_offer);
    receive_offer_done(ctx->term);
    free(ctx);
}

static void
drop(void *data, struct wl_data_device *wl_data_device)
{
    struct seat *seat = data;

    xassert(seat->clipboard.window != NULL);
    struct terminal *term = seat->clipboard.window->term;

    struct wl_clipboard *clipboard = &seat->clipboard;

    if (clipboard->mime_type == DATA_OFFER_MIME_UNSET) {
        LOG_WARN("compositor called data_device::drop() "
                 "even though we rejected the drag-and-drop");
        return;
    }

    struct dnd_context *ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct dnd_context){
        .term = term,
        .data_offer = clipboard->data_offer,
    };

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        free(ctx);
        return;
    }

    int read_fd = fds[0];
    int write_fd = fds[1];

    LOG_DBG("DnD drop: mime-type=%s", mime_type_map[clipboard->mime_type]);

    /* Give write-end of pipe to other client */
    wl_data_offer_receive(
        clipboard->data_offer, mime_type_map[clipboard->mime_type], write_fd);

    /* Don't keep our copy of the write-end open (or we'll never get EOF) */
    close(write_fd);

    term->is_sending_paste_data = true;

    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[200~", 6);

    begin_receive_clipboard(
        term, read_fd, clipboard->mime_type,
        &receive_dnd, &receive_dnd_done, ctx);

    /* data offer is now "owned" by the receive context */
    clipboard->data_offer = NULL;
    clipboard->mime_type = DATA_OFFER_MIME_UNSET;
}

static void
selection(void *data, struct wl_data_device *wl_data_device,
          struct wl_data_offer *offer)
{
    /* Selection offer from other client */
    struct seat *seat = data;
    if (offer == NULL)
        data_offer_reset(&seat->clipboard);
    else
        xassert(offer == seat->clipboard.data_offer);
}

const struct wl_data_device_listener data_device_listener = {
    .data_offer = &data_offer,
    .enter = &enter,
    .leave = &leave,
    .motion = &motion,
    .drop = &drop,
    .selection = &selection,
};

static void
primary_offer(void *data,
              struct zwp_primary_selection_offer_v1 *zwp_primary_selection_offer,
              const char *mime_type)
{
    LOG_DBG("primary offer: %s", mime_type);
    struct seat *seat = data;
    select_mime_type_for_offer(mime_type, &seat->primary.mime_type);
}

static const struct zwp_primary_selection_offer_v1_listener primary_selection_offer_listener = {
    .offer = &primary_offer,
};

static void
primary_offer_reset(struct wl_primary *primary)
{
    if (primary->data_offer != NULL) {
        zwp_primary_selection_offer_v1_destroy(primary->data_offer);
        primary->data_offer = NULL;
    }

    primary->mime_type = DATA_OFFER_MIME_UNSET;
}

static void
primary_data_offer(void *data,
                   struct zwp_primary_selection_device_v1 *zwp_primary_selection_device,
                   struct zwp_primary_selection_offer_v1 *offer)
{
    struct seat *seat = data;
    primary_offer_reset(&seat->primary);
    seat->primary.data_offer = offer;
    zwp_primary_selection_offer_v1_add_listener(
        offer, &primary_selection_offer_listener, seat);
}

static void
primary_selection(void *data,
                  struct zwp_primary_selection_device_v1 *zwp_primary_selection_device,
                  struct zwp_primary_selection_offer_v1 *offer)
{
    /* Selection offer from other client, for primary */

    struct seat *seat = data;
    if (offer == NULL)
        primary_offer_reset(&seat->primary);
    else
        xassert(seat->primary.data_offer == offer);
}

const struct zwp_primary_selection_device_v1_listener primary_selection_device_listener = {
    .data_offer = &primary_data_offer,
    .selection = &primary_selection,
};

