#include "commands.h"

#define LOG_MODULE "commands"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "grid.h"
#include "render.h"
#include "selection.h"
#include "terminal.h"
#include "url-mode.h"
#include "util.h"

void
cmd_scrollback_up(struct terminal *term, int rows)
{
    if (term->grid == &term->alt)
        return;
    if (urls_mode_is_active(term))
        return;

    const struct grid *grid = term->grid;
    const int view = grid->view;
    const int grid_rows = grid->num_rows;

    /* The view row number in scrollback relative coordinates. This is
     * the maximum number of rows we’re allowed to scroll */
    int sb_start = grid_sb_start_ignore_uninitialized(grid, term->rows);
    int view_sb_rel =
        grid_row_abs_to_sb_precalc_sb_start(grid, sb_start, view);

    rows = min(rows, view_sb_rel);
    if (rows == 0)
        return;

    int new_view = (view + grid_rows) - rows;
    new_view &= grid_rows - 1;

    xassert(new_view != view);
    xassert(grid->rows[new_view] != NULL);
#if defined(_DEBUG)
    for (int r = 0; r < term->rows; r++)
        xassert(grid->rows[(new_view + r) & (grid->num_rows - 1)] != NULL);
#endif

    LOG_DBG("scrollback UP: %d -> %d (offset = %d, rows = %d)",
            view, new_view, offset, grid_rows);

    selection_view_up(term, new_view);
    term->grid->view = new_view;

    if (rows < term->rows) {
        term_damage_scroll(
            term, DAMAGE_SCROLL_REVERSE_IN_VIEW,
            (struct scroll_region){0, term->rows}, rows);
        term_damage_rows_in_view(term, 0, rows - 1);
    } else
        term_damage_view(term);

    render_refresh_urls(term);
    render_refresh(term);
}

void
cmd_scrollback_down(struct terminal *term, int rows)
{
    if (term->grid == &term->alt)
        return;
    if (urls_mode_is_active(term))
        return;

    const struct grid *grid = term->grid;
    const int offset = grid->offset;
    const int view = grid->view;
    const int grid_rows = grid->num_rows;
    const int screen_rows = term->rows;

    const int scrollback_end = offset;

    /* Number of rows to scroll, without going past the scrollback end */
    int max_rows = 0;
    if (view <= scrollback_end)
        max_rows = scrollback_end - view;
    else
        max_rows = offset + (grid_rows - view);

    rows = min(rows, max_rows);
    if (rows == 0)
        return;

    int new_view = (view + rows) & (grid_rows - 1);

    xassert(new_view != view);
    xassert(grid->rows[new_view] != NULL);
#if defined(_DEBUG)
    for (int r = 0; r < term->rows; r++)
        xassert(grid->rows[(new_view + r) & (grid_rows - 1)] != NULL);
#endif

    LOG_DBG("scrollback DOWN: %d -> %d (offset = %d, rows = %d)",
            view, new_view, offset, grid_rows);

    selection_view_down(term, new_view);
    term->grid->view = new_view;

    if (rows < term->rows) {
        term_damage_scroll(
            term, DAMAGE_SCROLL_IN_VIEW,
            (struct scroll_region){0, term->rows}, rows);
        term_damage_rows_in_view(term, term->rows - rows, screen_rows - 1);
    } else
        term_damage_view(term);

    render_refresh_urls(term);
    render_refresh(term);
}
