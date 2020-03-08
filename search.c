#include "search.h"

#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "search"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "grid.h"
#include "input.h"
#include "misc.h"
#include "render.h"
#include "selection.h"
#include "shm.h"

#define max(x, y) ((x) > (y) ? (x) : (y))

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

static void
search_cancel_keep_selection(struct terminal *term)
{
    struct wl_window *win = term->window;
    if (win->search_sub_surface != NULL)
        wl_subsurface_destroy(win->search_sub_surface);
    if (win->search_surface != NULL)
        wl_surface_destroy(win->search_surface);

    win->search_surface = NULL;
    win->search_sub_surface = NULL;

    free(term->search.buf);
    term->search.buf = NULL;
    term->search.len = 0;
    term->search.sz = 0;
    term->search.cursor = 0;
    term->search.match = (struct coord){-1, -1};
    term->search.match_len = 0;
    term->is_searching = false;
    term->render.search_glyph_offset = 0;

    term_xcursor_update(term);
    render_refresh(term);
}

void
search_begin(struct terminal *term)
{
    LOG_DBG("search: begin");

    search_cancel_keep_selection(term);
    selection_cancel(term);

    /* On-demand instantiate wayland surface */
    struct wl_window *win = term->window;
    struct wayland *wayl = term->wl;
    win->search_surface = wl_compositor_create_surface(wayl->compositor);
    wl_surface_set_user_data(win->search_surface, term->window);

    win->search_sub_surface = wl_subcompositor_get_subsurface(
        wayl->sub_compositor, win->search_surface, win->surface);
    wl_subsurface_set_sync(win->search_sub_surface);

    term->search.original_view = term->grid->view;
    term->search.view_followed_offset = term->grid->view == term->grid->offset;
    term->is_searching = true;

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
    int old_view = term->grid->view;
    int new_view = start_row;

    /* Prevent scrolling in uninitialized rows */
    bool all_initialized = false;
    do {
        all_initialized = true;

        for (int i = 0; i < term->rows; i++) {
            int row_no = (new_view + i) % term->grid->num_rows;
            if (term->grid->rows[row_no] == NULL) {
                all_initialized = false;
                new_view--;
                break;
            }
        }
    } while (!all_initialized);

    /* Don't scroll past scrollback history */
    int end = (term->grid->offset + term->rows - 1) % term->grid->num_rows;
    if (end >= term->grid->offset) {
        /* Not wrapped */
        if (new_view >= term->grid->offset && new_view <= end)
            new_view = term->grid->offset;
    } else {
        if (new_view >= term->grid->offset || new_view <= end)
            new_view = term->grid->offset;
    }

    /* Update view */
    term->grid->view = new_view;
    if (new_view != old_view)
        term_damage_view(term);

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

        assert(selection_row >= 0 &&
               selection_row < term->grid->num_rows);
        selection_start(term, start_col, selection_row, SELECTION_NORMAL);
    }

    /* Update selection endpoint */
    {
        int selection_row = end_row - term->grid->view;
        while (selection_row < 0)
            selection_row += term->grid->num_rows;

        assert(selection_row >= 0 &&
               selection_row < term->grid->num_rows);
        selection_update(term, end_col, selection_row);
    }
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
    size_t len __attribute__((unused)) = term->search.match_len;

    assert((len == 0 && start_row == -1 && start_col == -1) ||
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

#define ROW_DEC(_r) ((_r) = ((_r) - 1 + term->grid->num_rows) % term->grid->num_rows)
#define ROW_INC(_r) ((_r) = ((_r) + 1) % term->grid->num_rows)

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

            if (wcsncasecmp(&row->cells[start_col].wc, term->search.buf, 1) != 0)
                continue;

            /*
             * Got a match on the first letter. Now we'll see if the
             * rest of the search buffer matches.
             */

            LOG_DBG("search: initial match at row=%d, col=%d", start_row, start_col);

            int end_row = start_row;
            int end_col = start_col;
            size_t match_len = 0;

            for (size_t i = 0; i < term->search.len; i++, match_len++) {
                if (end_col >= term->cols) {
                    if (end_row + 1 > grid_row_absolute(term->grid, term->grid->offset + term->rows - 1)) {
                        /* Don't continue past end of the world */
                        break;
                    }

                    end_row++;
                    end_col = 0;
                    row = term->grid->rows[end_row];
                }

                if (wcsncasecmp(&row->cells[end_col].wc, &term->search.buf[i], 1) != 0)
                    break;

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

static void
search_match_to_end_of_word(struct terminal *term, bool spaces_only)
{
    if (term->search.match_len == 0)
        return;

    assert(term->search.match.row != -1);
    assert(term->search.match.col != -1);

    int end_row = term->search.match.row;
    int end_col = term->search.match.col;
    size_t len = term->search.match_len;

    /* Calculate end coord - note: assumed to be valid */
    for (size_t i = 0; i < len; i++) {
        if (++end_col >= term->cols) {
            end_row = (end_row + 1) % term->grid->num_rows;
            end_col = 0;
        }
    }

    tll(wchar_t) new_chars = tll_init();

    /* Always append at least one character *if* possible */
    bool first = true;

    for (size_t r = 0;
         r < term->grid->num_rows;
         end_row = (end_row + 1) % term->grid->num_rows, r++)
    {
        const struct row *row = term->grid->rows[end_row];
        if (row == NULL)
            break;

        bool done = false;
        for (; end_col < term->cols; end_col++) {
            wchar_t wc = row->cells[end_col].wc;
            if (wc == 0 || (!first && !isword(wc, spaces_only))) {
                done = true;
                break;
            }

            first = false;
            tll_push_back(new_chars, wc);
        }

        if (done)
            break;

        end_col = 0;
    }

    if (tll_length(new_chars) == 0)
        return;

    if (!search_ensure_size(term, term->search.len + tll_length(new_chars)))
        return;

    /* Keep cursor at the end, but don't move it if not */
    bool move_cursor = term->search.cursor == term->search.len;

    /* Append newly found characters to the search buffer */
    tll_foreach(new_chars, it)
        term->search.buf[term->search.len++] = it->item;
    term->search.buf[term->search.len] = L'\0';

    if (move_cursor)
        term->search.cursor += tll_length(new_chars);

    tll_free(new_chars);

    search_update_selection(
        term, term->search.match.row, term->search.match.col, end_row, end_col);
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

    assert(cursor == term->search.len || iswspace(term->search.buf[cursor - 1]));

    /* Now skip past whitespace, so that we end up at the beginning of
     * the next word */
    while (cursor < term->search.len) {
        if (!iswspace(term->search.buf[cursor++]))
            break;
    }

    assert(cursor == term->search.len || !iswspace(term->search.buf[cursor - 1]));

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

    assert(cursor == 0 || !iswspace(term->search.buf[cursor]));

    /* Now eat non-whitespace. This is the word we're skipping past */
    while (cursor > 0) {
        if (iswspace(term->search.buf[--cursor]))
            break;
    }

    assert(cursor == 0 || iswspace(term->search.buf[cursor]));
    if (cursor > 0 && iswspace(term->search.buf[cursor]))
        cursor++;

    return term->search.cursor - cursor;
}

void
search_input(struct terminal *term, uint32_t key, xkb_keysym_t sym,
             xkb_mod_mask_t mods, uint32_t serial)
{
    LOG_DBG("search: input: sym=%d/0x%x, mods=0x%08x", sym, sym, mods);

    const xkb_mod_mask_t ctrl = 1 << term->wl->kbd.mod_ctrl;
    const xkb_mod_mask_t alt = 1 << term->wl->kbd.mod_alt;
    const xkb_mod_mask_t shift = 1 << term->wl->kbd.mod_shift;
    //const xkb_mod_mask_t meta = 1 << term->wl->kbd.mod_meta;

    enum xkb_compose_status compose_status = xkb_compose_state_get_status(
        term->wl->kbd.xkb_compose_state);

    /*
     * User configurable bindings
     */
    tll_foreach(term->wl->kbd.bindings.search, it) {
        if (it->item.mods == mods && it->item.sym == sym) {
            input_execute_binding(term, it->item.action, serial);
            return;
        }
    }

    /* Cancel search */
    if ((mods == 0 && sym == XKB_KEY_Escape) ||
        (mods == ctrl && sym == XKB_KEY_g))
    {
        if (term->search.view_followed_offset)
            term->grid->view = term->grid->offset;
        else
            term->grid->view = term->search.original_view;
        term_damage_view(term);
        search_cancel(term);
        return;
    }

    /* "Commit" search - copy selection to primary and cancel search */
    else if (mods == 0 && sym == XKB_KEY_Return) {
        selection_finalize(term, term->wl->input_serial);
        search_cancel_keep_selection(term);
        return;
    }

    else if (mods == ctrl && sym == XKB_KEY_r) {
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
    }

    else if (mods == ctrl && sym == XKB_KEY_s) {
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
    }

    else if ((mods == 0 && sym == XKB_KEY_Left) ||
             (mods == ctrl && sym == XKB_KEY_b))
    {
        if (term->search.cursor > 0)
            term->search.cursor--;
    }

    else if ((mods == ctrl && sym == XKB_KEY_Left) ||
             (mods == alt && sym == XKB_KEY_b))
    {
        size_t diff = distance_prev_word(term);
        term->search.cursor -= diff;
        assert(term->search.cursor >= 0);
        assert(term->search.cursor <= term->search.len);
    }

    else if ((mods == 0 && sym == XKB_KEY_Right) ||
             (mods == ctrl && sym == XKB_KEY_f))
    {
        if (term->search.cursor < term->search.len)
            term->search.cursor++;
    }

    else if ((mods == ctrl && sym == XKB_KEY_Right) ||
             (mods == alt && sym == XKB_KEY_f))
    {
        size_t diff = distance_next_word(term);
        term->search.cursor += diff;
        assert(term->search.cursor >= 0);
        assert(term->search.cursor <= term->search.len);
    }

    else if ((mods == 0 && sym == XKB_KEY_Home) ||
             (mods == ctrl && sym == XKB_KEY_a))
        term->search.cursor = 0;

    else if ((mods == 0 && sym == XKB_KEY_End) ||
             (mods == ctrl && sym == XKB_KEY_e))
        term->search.cursor = term->search.len;

    else if (mods == 0 && sym == XKB_KEY_BackSpace) {
        if (term->search.cursor > 0) {
            memmove(
                &term->search.buf[term->search.cursor - 1],
                &term->search.buf[term->search.cursor],
                (term->search.len - term->search.cursor) * sizeof(wchar_t));
            term->search.cursor--;
            term->search.buf[--term->search.len] = L'\0';
        }
    }

    else if ((mods == alt || mods == ctrl) && sym == XKB_KEY_BackSpace) {
        size_t diff = distance_prev_word(term);
        size_t old_cursor = term->search.cursor;
        size_t new_cursor = old_cursor - diff;

        memmove(&term->search.buf[new_cursor],
                &term->search.buf[old_cursor],
                (term->search.len - old_cursor) * sizeof(wchar_t));

        term->search.len -= diff;
        term->search.cursor = new_cursor;
    }

    else if ((mods == alt && sym == XKB_KEY_d) ||
             (mods == ctrl && sym == XKB_KEY_Delete)) {
        size_t diff = distance_next_word(term);
        size_t cursor = term->search.cursor;

        memmove(&term->search.buf[cursor],
                &term->search.buf[cursor + diff],
                (term->search.len - (cursor + diff)) * sizeof(wchar_t));

        term->search.len -= diff;
    }

    else if (mods == 0 && sym == XKB_KEY_Delete) {
        if (term->search.cursor < term->search.len) {
            memmove(
                &term->search.buf[term->search.cursor],
                &term->search.buf[term->search.cursor + 1],
                (term->search.len - term->search.cursor - 1) * sizeof(wchar_t));
            term->search.buf[--term->search.len] = L'\0';
        }
    }

    else if (mods == ctrl && sym == XKB_KEY_w)
        search_match_to_end_of_word(term, false);

    else if (mods == (ctrl | shift) && sym == XKB_KEY_W)
        search_match_to_end_of_word(term, true);

    else {
        uint8_t buf[64] = {0};
        int count = 0;

        if (compose_status == XKB_COMPOSE_COMPOSED) {
            count = xkb_compose_state_get_utf8(
                term->wl->kbd.xkb_compose_state, (char *)buf, sizeof(buf));
            xkb_compose_state_reset(term->wl->kbd.xkb_compose_state);
        } else if (compose_status == XKB_COMPOSE_CANCELLED) {
            count = 0;
        } else {
            count = xkb_state_key_get_utf8(
                term->wl->kbd.xkb_state, key, (char *)buf, sizeof(buf));
        }

        const char *src = (const char *)buf;
        mbstate_t ps = {0};
        size_t wchars = mbsnrtowcs(NULL, &src, count, 0, &ps);

        if (wchars == -1) {
            LOG_ERRNO("failed to convert %.*s to wchars", count, buf);
            return;
        }

        if (!search_ensure_size(term, term->search.len + wchars))
            return;

        assert(term->search.len + wchars < term->search.sz);

        memmove(&term->search.buf[term->search.cursor + wchars],
                &term->search.buf[term->search.cursor],
                (term->search.len - term->search.cursor) * sizeof(wchar_t));

        memset(&ps, 0, sizeof(ps));
        mbsnrtowcs(&term->search.buf[term->search.cursor], &src, count,
                   wchars, &ps);

        term->search.len += wchars;
        term->search.cursor += wchars;
        term->search.buf[term->search.len] = L'\0';
    }

    LOG_DBG("search: buffer: %S", term->search.buf);
    search_find_next(term);
    render_refresh_search(term);
}
