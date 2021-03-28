#include "search.h"

#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "search"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "extract.h"
#include "grid.h"
#include "input.h"
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
    int view_end = (new_view + term->rows - 1) & (term->grid->num_rows - 1);

    if (term->grid->rows[new_view] == NULL) {
        while (term->grid->rows[new_view] == NULL)
            new_view = (new_view + 1) & (term->grid->num_rows - 1);
    }

    else if (term->grid->rows[view_end] == NULL) {
        while (term->grid->rows[view_end] == NULL) {
            new_view--;
            if (new_view < 0)
                new_view += term->grid->num_rows;
            view_end = (new_view + term->rows - 1) & (term->grid->num_rows - 1);
        }
    }

#if defined(_DEBUG)
    for (size_t r = 0; r < term->rows; r++)
        xassert(term->grid->rows[(new_view + r) & (term->grid->num_rows - 1)] != NULL);
#endif

    return new_view;
}

static bool
search_ensure_size(struct terminal *term, size_t wanted_size)
{
    while (wanted_size >= term->search.sz) {
        size_t new_sz = term->search.sz == 0 ? 64 : term->search.sz * 2;
        wchar_t *new_buf = realloc(term->search.buf, new_sz * sizeof(term->search.buf[0]));

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
    int scrollback_start = term->grid->offset + term->rows;
    int rebased_row = abs_row_no - scrollback_start + term->grid->num_rows;
    rebased_row &= term->grid->num_rows - 1;

    return rebased_row == 0;
}

static void
search_cancel_keep_selection(struct terminal *term)
{
    struct wl_window *win = term->window;
    wayl_win_subsurface_destroy(&win->search);

    free(term->search.buf);
    term->search.buf = NULL;
    term->search.len = 0;
    term->search.sz = 0;
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
    bool ret = wayl_win_subsurface_new(term->window, &term->window->search);
    xassert(ret);

    term->search.original_view = term->grid->view;
    term->search.view_followed_offset = term->grid->view == term->grid->offset;
    term->is_searching = true;

    term->search.len = 0;
    term->search.sz = 64;
    term->search.buf = xmalloc(term->search.sz * sizeof(term->search.buf[0]));
    term->search.buf[0] = L'\0';

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

static void
search_update_selection(struct terminal *term,
                        int start_row, int start_col,
                        int end_row, int end_col)
{
    bool move_viewport = true;

    int view_end = (term->grid->view + term->rows - 1) & (term->grid->num_rows - 1);
    if (view_end >= term->grid->view) {
        /* Viewport does *not* wrap around */
        if (start_row >= term->grid->view && end_row <= view_end)
            move_viewport = false;
    } else {
        /* Viewport wraps */
        if (start_row >= term->grid->view || end_row <= view_end)
            move_viewport = false;
    }

    if (move_viewport) {
        int old_view = term->grid->view;
        int new_view = start_row - term->rows / 2;

        while (new_view < 0)
            new_view += term->grid->num_rows;

        new_view = ensure_view_is_allocated(term, new_view);

        /* Don't scroll past scrollback history */
        int end = (term->grid->offset + term->rows - 1) & (term->grid->num_rows - 1);
        if (end >= term->grid->offset) {
            /* Not wrapped */
            if (new_view >= term->grid->offset && new_view <= end)
                new_view = term->grid->offset;
        } else {
            if (new_view >= term->grid->offset || new_view <= end)
                new_view = term->grid->offset;
        }

#if defined(_DEBUG)
        /* Verify all to-be-visible rows have been allocated */
        for (int r = 0; r < term->rows; r++)
            xassert(term->grid->rows[(new_view + r) & (term->grid->num_rows - 1)] != NULL);
#endif

        /* Update view */
        term->grid->view = new_view;
        if (new_view != old_view)
            term_damage_view(term);
    }

    /* Selection endpoint is inclusive */
    if (--end_col < 0) {
        end_col = term->cols - 1;
        end_row--;
    }

    /* Begin a new selection if the start coords changed */
    if (start_row != term->search.match.row ||
        start_col != term->search.match.col)
    {
        int selection_row = start_row - term->grid->view;
        while (selection_row < 0)
            selection_row += term->grid->num_rows;

        xassert(selection_row >= 0 &&
               selection_row < term->grid->num_rows);
        selection_start(
            term, start_col, selection_row, SELECTION_CHAR_WISE, false);
    }

    /* Update selection endpoint */
    {
        int selection_row = end_row - term->grid->view;
        while (selection_row < 0)
            selection_row += term->grid->num_rows;

        xassert(selection_row >= 0 &&
               selection_row < term->grid->num_rows);
        selection_update(term, end_col, selection_row);
    }
}

static ssize_t
matches_cell(const struct terminal *term, const struct cell *cell, size_t search_ofs)
{
    assert(search_ofs < term->search.len);

    wchar_t base = cell->wc;
    const struct composed *composed = NULL;

    if (base >= CELL_COMB_CHARS_LO &&
        base < (CELL_COMB_CHARS_LO + term->composed_count))
    {
        composed = &term->composed[base - CELL_COMB_CHARS_LO];
        base = composed->base;
    }

    if (composed == NULL && base == 0 && term->search.buf[search_ofs] == L' ')
        return 1;

    if (wcsncasecmp(&base, &term->search.buf[search_ofs], 1) != 0)
        return -1;

    if (composed != NULL) {
        if (search_ofs + 1 + composed->count > term->search.len)
            return -1;

        for (size_t j = 0; j < composed->count; j++) {
            if (composed->combining[j] != term->search.buf[search_ofs + 1 + j])
                return -1;
        }
    }

    return composed != NULL ? 1 + composed->count : 1;
}

static void
search_find_next(struct terminal *term)
{
    bool backward = term->search.direction == SEARCH_BACKWARD;
    term->search.direction = SEARCH_BACKWARD;

    if (term->search.len == 0) {
        term->search.match = (struct coord){-1, -1};
        term->search.match_len = 0;
        selection_cancel(term);
        return;
    }

    int start_row = term->search.match.row;
    int start_col = term->search.match.col;
    size_t len = term->search.match_len;

    xassert((len == 0 && start_row == -1 && start_col == -1) ||
           (len > 0 && start_row >= 0 && start_col >= 0));

    if (len == 0) {
        if (backward) {
            start_row = grid_row_absolute_in_view(term->grid, term->rows - 1);
            start_col = term->cols - 1;
        } else {
            start_row = grid_row_absolute_in_view(term->grid, 0);
            start_col = 0;
        }
    }

    LOG_DBG("search: update: %s: starting at row=%d col=%d (offset = %d, view = %d)",
            backward ? "backward" : "forward", start_row, start_col,
            term->grid->offset, term->grid->view);

#define ROW_DEC(_r) ((_r) = ((_r) - 1 + term->grid->num_rows) & (term->grid->num_rows - 1))
#define ROW_INC(_r) ((_r) = ((_r) + 1) & (term->grid->num_rows - 1))

    /* Scan backward from current end-of-output */
    /* TODO: don't search "scrollback" in alt screen? */
    for (size_t r = 0;
         r < term->grid->num_rows;
         backward ? ROW_DEC(start_row) : ROW_INC(start_row), r++)
    {
        for (;
             backward ? start_col >= 0 : start_col < term->cols;
             backward ? start_col-- : start_col++)
        {
            const struct row *row = term->grid->rows[start_row];
            if (row == NULL)
                continue;

            if (matches_cell(term, &row->cells[start_col], 0) < 0)
                continue;

            /*
             * Got a match on the first letter. Now we'll see if the
             * rest of the search buffer matches.
             */

            LOG_DBG("search: initial match at row=%d, col=%d", start_row, start_col);

            int end_row = start_row;
            int end_col = start_col;
            size_t match_len = 0;

            for (size_t i = 0; i < term->search.len;) {
                if (end_col >= term->cols) {
                    end_row++;
                    end_col = 0;

                    if (has_wrapped_around(term, end_row))
                        break;

                    row = term->grid->rows[end_row];
                }

                if (row->cells[end_col].wc >= CELL_SPACER) {
                    end_col++;
                    continue;
                }

                ssize_t additional_chars = matches_cell(term, &row->cells[end_col], i);
                if (additional_chars < 0)
                    break;

                i += additional_chars;
                match_len += additional_chars;
                end_col++;
            }

            if (match_len != term->search.len) {
                /* Didn't match (completely) */
                continue;
            }

            /*
             * We matched the entire buffer. Move view to ensure the
             * match is visible, create a selection and return.
             */
            search_update_selection(term, start_row, start_col, end_row, end_col);

            /* Update match state */
            term->search.match.row = start_row;
            term->search.match.col = start_col;
            term->search.match_len = match_len;

            return;
        }

        start_col = backward ? term->cols - 1 : 0;
    }

    /* No match */
    LOG_DBG("no match");
    term->search.match = (struct coord){-1, -1};
    term->search.match_len = 0;
    selection_cancel(term);
#undef ROW_DEC
}

void
search_add_chars(struct terminal *term, const char *src, size_t count)
{
    const char *_src = src;
    mbstate_t ps = {0};
    size_t wchars = mbsnrtowcs(NULL, &_src, count, 0, &ps);

    if (wchars == -1) {
        LOG_ERRNO("failed to convert %.*s to wchars", (int)count, src);
        return;
    }

    _src = src;
    ps = (mbstate_t){0};
    wchar_t wcs[wchars + 1];
    mbsnrtowcs(wcs, &_src, count, wchars, &ps);

    /* Strip non-printable characters */
    for (size_t i = 0, j = 0, orig_wchars = wchars; i < orig_wchars; i++) {
        if (iswprint(wcs[i]))
            wcs[j++] = wcs[i];
        else
            wchars--;
    }

    if (!search_ensure_size(term, term->search.len + wchars))
        return;

    xassert(term->search.len + wchars < term->search.sz);

    memmove(&term->search.buf[term->search.cursor + wchars],
            &term->search.buf[term->search.cursor],
            (term->search.len - term->search.cursor) * sizeof(wchar_t));

    memcpy(&term->search.buf[term->search.cursor], wcs, wchars * sizeof(wchar_t));

    term->search.len += wchars;
    term->search.cursor += wchars;
    term->search.buf[term->search.len] = L'\0';
}

static void
search_match_to_end_of_word(struct terminal *term, bool spaces_only)
{
    if (term->search.match_len == 0)
        return;

    xassert(term->selection.end.row != -1);

    const bool move_cursor = term->search.cursor == term->search.len;

    const struct coord old_end = term->selection.end;
    struct coord new_end = old_end;

    /* First character to consider is the *next* character */
    if (++new_end.col >= term->cols) {
        new_end.row = (new_end.row + 1) & (term->grid->num_rows - 1);
        new_end.col = 0;

        if (has_wrapped_around(term, new_end.row))
            return;
    }

    xassert(term->grid->rows[new_end.row] != NULL);

    new_end.row -= term->grid->view;
    selection_find_word_boundary_right(term, &new_end, spaces_only);
    new_end.row += term->grid->view;

    if (new_end.row == old_end.row && new_end.col == old_end.col)
        return;

    struct coord pos = old_end;
    const struct row *row = term->grid->rows[pos.row];

    struct extraction_context *ctx = extract_begin(SELECTION_NONE);
    if (ctx == NULL)
        return;

    do {
        if (++pos.col >= term->cols) {
            pos.row = (pos.row + 1) & (term->grid->num_rows - 1);
            pos.col = 0;
            row = term->grid->rows[pos.row];
        }
        if (!extract_one(term, row, &row->cells[pos.col], pos.col, ctx))
            break;
    } while (pos.col != new_end.col || pos.row != new_end.row);

    wchar_t *new_text;
    size_t new_len;

    if (!extract_finish_wide(ctx, false, &new_text, &new_len))
        return;

    if (!search_ensure_size(term, term->search.len + new_len))
        return;

    for (size_t i = 0; i < new_len; i++)
        term->search.buf[term->search.len++] = new_text[i];

    term->search.buf[term->search.len] = L'\0';
    free(new_text);

    if (move_cursor)
        term->search.cursor = term->search.len;

    search_update_selection(
        term, term->search.match.row, term->search.match.col,
        new_end.row, new_end.col);
}

static size_t
distance_next_word(const struct terminal *term)
{
    size_t cursor = term->search.cursor;

    /* First eat non-whitespace. This is the word we're skipping past */
    while (cursor < term->search.len) {
        if (iswspace(term->search.buf[cursor++]))
            break;
    }

    xassert(cursor == term->search.len || iswspace(term->search.buf[cursor - 1]));

    /* Now skip past whitespace, so that we end up at the beginning of
     * the next word */
    while (cursor < term->search.len) {
        if (!iswspace(term->search.buf[cursor++]))
            break;
    }

    xassert(cursor == term->search.len || !iswspace(term->search.buf[cursor - 1]));

    if (cursor < term->search.len && !iswspace(term->search.buf[cursor]))
        cursor--;

    return cursor - term->search.cursor;
}

static size_t
distance_prev_word(const struct terminal *term)
{
    int cursor = term->search.cursor;

    /* First, eat whitespace prefix */
    while (cursor > 0) {
        if (!iswspace(term->search.buf[--cursor]))
            break;
    }

    xassert(cursor == 0 || !iswspace(term->search.buf[cursor]));

    /* Now eat non-whitespace. This is the word we're skipping past */
    while (cursor > 0) {
        if (iswspace(term->search.buf[--cursor]))
            break;
    }

    xassert(cursor == 0 || iswspace(term->search.buf[cursor]));
    if (cursor > 0 && iswspace(term->search.buf[cursor]))
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

    LOG_DBG("search: buffer: %ls", term->search.buf);
    search_find_next(term);
    render_refresh_search(term);
}

static bool
execute_binding(struct seat *seat, struct terminal *term,
                enum bind_action_search action, uint32_t serial,
                bool *update_search_result, bool *redraw)
{
    *update_search_result = *redraw = false;

    switch (action) {
    case BIND_ACTION_SEARCH_NONE:
        return false;

    case BIND_ACTION_SEARCH_CANCEL:
        if (term->search.view_followed_offset)
            term->grid->view = term->grid->offset;
        else {
            term->grid->view = ensure_view_is_allocated(
                term, term->search.original_view);
        }
        term_damage_view(term);
        search_cancel(term);
        return true;

    case BIND_ACTION_SEARCH_COMMIT:
        selection_finalize(seat, term, serial);
        search_cancel_keep_selection(term);
        return true;

    case BIND_ACTION_SEARCH_FIND_PREV:
        if (term->search.match_len > 0) {
            int new_col = term->search.match.col - 1;
            int new_row = term->search.match.row;

            if (new_col < 0) {
                new_col = term->cols - 1;
                new_row--;
            }

            if (new_row >= 0) {
                term->search.match.col = new_col;
                term->search.match.row = new_row;
            }
        }
        *update_search_result = *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_FIND_NEXT:
        if (term->search.match_len > 0) {
            int new_col = term->search.match.col + 1;
            int new_row = term->search.match.row;

            if (new_col >= term->cols) {
                new_col = 0;
                new_row++;
            }

            if (new_row < term->grid->num_rows) {
                term->search.match.col = new_col;
                term->search.match.row = new_row;
                term->search.direction = SEARCH_FORWARD;
             }
        }
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
                (term->search.len - term->search.cursor) * sizeof(wchar_t));
            term->search.cursor--;
            term->search.buf[--term->search.len] = L'\0';
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
                    (term->search.len - old_cursor) * sizeof(wchar_t));

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
                (term->search.len - term->search.cursor - 1) * sizeof(wchar_t));
            term->search.buf[--term->search.len] = L'\0';
            *update_search_result = *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_DELETE_NEXT_WORD: {
        size_t diff = distance_next_word(term);
        size_t cursor = term->search.cursor;

        if (diff > 0) {
            memmove(&term->search.buf[cursor],
                    &term->search.buf[cursor + diff],
                    (term->search.len - (cursor + diff)) * sizeof(wchar_t));

            term->search.len -= diff;
            *update_search_result = *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_EXTEND_WORD:
        search_match_to_end_of_word(term, false);
        *update_search_result = *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_EXTEND_WORD_WS:
        search_match_to_end_of_word(term, true);
        *update_search_result = *redraw = true;
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
search_input(struct seat *seat, struct terminal *term, uint32_t key,
             xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
             const xkb_keysym_t *raw_syms, size_t raw_count,
             uint32_t serial)
{
    LOG_DBG("search: input: sym=%d/0x%x, mods=0x%08x", sym, sym, mods);

    enum xkb_compose_status compose_status = xkb_compose_state_get_status(
        seat->kbd.xkb_compose_state);

    bool update_search_result = false;
    bool redraw = false;

    /* Key bindings */
    tll_foreach(seat->kbd.bindings.search, it) {
        const struct key_binding *bind = &it->item;

        /* Match translated symbol */
        if (bind->sym == sym &&
            bind->mods == (mods & ~consumed)) {

            if (execute_binding(seat, term, bind->action, serial,
                                &update_search_result, &redraw))
            {
                goto update_search;
            }
            return;
        }

        if (bind->mods != mods)
            continue;

        /* Match untranslated symbols */
        for (size_t i = 0; i < raw_count; i++) {
            if (bind->sym == raw_syms[i]) {
                if (execute_binding(seat, term, bind->action, serial,
                                    &update_search_result, &redraw))
                {
                    goto update_search;
                }
                return;
            }
        }

        /* Match raw key code */
        tll_foreach(bind->key_codes, code) {
            if (code->item == key) {
                if (execute_binding(seat, term, bind->action, serial,
                                    &update_search_result, &redraw))
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

    if (count == 0)
        return;

    search_add_chars(term, (const char *)buf, count);

update_search:
    LOG_DBG("search: buffer: %ls", term->search.buf);
    if (update_search_result)
        search_find_next(term);
    if (redraw)
        render_refresh_search(term);
}
