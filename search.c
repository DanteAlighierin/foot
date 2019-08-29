#include "search.h"

#include <wchar.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "search"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "grid.h"
#include "render.h"
#include "selection.h"
#include "shm.h"

#define max(x, y) ((x) > (y) ? (x) : (y))

static void
search_cancel_keep_selection(struct terminal *term)
{
    wl_surface_attach(term->wl.search_surface, NULL, 0, 0);
    wl_surface_commit(term->wl.search_surface);

    free(term->search.buf);
    term->search.buf = NULL;
    term->search.len = 0;
    term->search.sz = 0;
    term->search.match = (struct coord){-1, -1};
    term->search.match_len = 0;
    term->is_searching = false;

    render_refresh(term);
}

void
search_begin(struct terminal *term)
{
    LOG_DBG("search: begin");

    search_cancel_keep_selection(term);
    selection_cancel(term);

    term->search.original_view = term->grid->view;
    term->search.view_followed_offset = term->grid->view == term->grid->offset;
    term->is_searching = true;

    render_search_box(term);
    render_refresh(term);
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
search_update(struct terminal *term)
{
    if (term->search.len == 0) {
        term->search.match = (struct coord){-1, -1};
        term->search.match_len = 0;
        selection_cancel(term);
        render_search_box(term);
        return;
    }

    int start_row = term->search.match.row;
    int start_col = term->search.match.col;
    size_t len __attribute__((unused)) = term->search.match_len;

    assert((len == 0 && start_row == -1 && start_col == -1) ||
           (len > 0 && start_row >= 0 && start_col >= 0));

    if (len == 0) {
        start_row = grid_row_absolute_in_view(term->grid, term->rows - 1);
        start_col = term->cols - 1;
    }

    LOG_DBG("search: update: starting at row=%d col=%d (offset = %d, view = %d)",
            start_row, start_col, term->grid->offset, term->grid->view);

#define ROW_DEC(_r) ((_r) = ((_r) - 1 + term->grid->num_rows) % term->grid->num_rows)

    /* Scan backward from current end-of-output */
    /* TODO: don't search "scrollback" in alt screen? */
    for (size_t r = 0; r < term->grid->num_rows; ROW_DEC(start_row), r++) {
        const struct row *row = term->grid->rows[start_row];
        if (row == NULL)
            continue;

        for (; start_col >= 0; start_col--) {
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
                if (wcsncasecmp(&row->cells[end_col].wc, &term->search.buf[i], 1) != 0)
                    break;

                if (++end_col >= term->cols) {
                    if (end_row + 1 > grid_row_absolute(term->grid, term->grid->offset + term->rows - 1)) {
                        /* Don't continue past end of the world */
                        break;
                    }

                    end_row++;
                    end_col = 0;
                    row = term->grid->rows[end_row];
                }
            }

            if (match_len != term->search.len) {
                /* Didn't match (completely) */
                continue;
            }

            /*
             * We matched the entire buffer. Move view to ensure the
             * match is visible, create a selection and return.
             */

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
                start_row--;
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
                selection_start(term, start_col, selection_row);
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

            /* Update match state */
            term->search.match.row = start_row;
            term->search.match.col = start_col;
            term->search.match_len = match_len;

            render_search_box(term);
            return;
        }

        start_col = term->cols - 1;
    }

    /* No match */
    LOG_DBG("no match");
    term->search.match = (struct coord){-1, -1};
    term->search.match_len = 0;
    selection_cancel(term);
    render_search_box(term);
#undef ROW_DEC
}

void
search_input(struct terminal *term, uint32_t key, xkb_keysym_t sym, xkb_mod_mask_t mods)
{
    LOG_DBG("search: input: sym=%d/0x%x, mods=0x%08x", sym, sym, mods);

    const xkb_mod_mask_t ctrl = 1 << term->kbd.mod_ctrl;
    //const xkb_mod_mask_t alt = 1 << term->kbd.mod_alt;
    //const xkb_mod_mask_t shift = 1 << term->kbd.mod_shift;
    //const xkb_mod_mask_t meta = 1 << term->kbd.mod_meta;

    enum xkb_compose_status compose_status = xkb_compose_state_get_status(
        term->kbd.xkb_compose_state);

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
        selection_finalize(term, term->input_serial);
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

    else if (mods == 0 && sym == XKB_KEY_BackSpace) {
        if (term->search.len > 0)
            term->search.buf[--term->search.len] = L'\0';
    }

    else {
        uint8_t buf[64] = {0};
        int count = 0;

        if (compose_status == XKB_COMPOSE_COMPOSED) {
            count = xkb_compose_state_get_utf8(
                term->kbd.xkb_compose_state, (char *)buf, sizeof(buf));
            xkb_compose_state_reset(term->kbd.xkb_compose_state);
        } else {
            count = xkb_state_key_get_utf8(
                term->kbd.xkb_state, key, (char *)buf, sizeof(buf));
        }

        const char *src = (const char *)buf;
        mbstate_t ps = {0};
        size_t wchars = mbsnrtowcs(NULL, &src, count, 0, &ps);

        if (wchars == -1) {
            LOG_ERRNO("failed to convert %.*s to wchars", count, buf);
            return;
        }

        while (term->search.len + wchars >= term->search.sz) {
            size_t new_sz = term->search.sz == 0 ? 64 : term->search.sz * 2;
            wchar_t *new_buf = realloc(term->search.buf, new_sz * sizeof(term->search.buf[0]));

            if (new_buf == NULL) {
                LOG_ERRNO("failed to resize search buffer");
                return;
            }

            term->search.buf = new_buf;
            term->search.sz = new_sz;
        }

        assert(term->search.len + wchars < term->search.sz);

        memset(&ps, 0, sizeof(ps));
        mbsnrtowcs(&term->search.buf[term->search.len], &src, count,
                   term->search.sz - term->search.len - 1, &ps);

        term->search.len += wchars;
        term->search.buf[term->search.len] = L'\0';
    }

    LOG_INFO("search: buffer: %S", term->search.buf);
    search_update(term);
    render_refresh(term);
}
