#include "commands.h"

#define LOG_MODULE "commands"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "grid.h"
#include "render.h"
#include "selection.h"
#include "terminal.h"
#include "util.h"

void
cmd_scrollback_up(struct terminal *term, int rows)
{
    if (term->grid == &term->alt)
        return;

    if (term->mouse_tracking != MOUSE_NONE)
        return;

    rows = min(rows, term->rows);
    assert(term->grid->offset >= 0);

    int new_view = term->grid->view - rows;
    while (new_view < 0)
        new_view += term->grid->num_rows;
    new_view %= term->grid->num_rows;

    assert(new_view >= 0);
    assert(new_view < term->grid->num_rows);

    /* Avoid scrolling in uninitialized rows */
    while (term->grid->rows[new_view] == NULL)
        new_view = (new_view + 1) % term->grid->num_rows;

    if (new_view == term->grid->view) {
        /*
         *  This happens when scrolling up in a newly opened terminal;
         *  every single line (except those already visible) are
         *  uninitalized, and the loop above will bring us back to
         *  where we started.
         */
        return;
    }

    /* Don't scroll past scrollback history */
    int end = (term->grid->offset + term->rows - 1) % term->grid->num_rows;
    if (end >= term->grid->offset) {
        /* Not wrapped */
        if (new_view >= term->grid->offset && new_view <= end)
            new_view = (end + 1) % term->grid->num_rows;
    } else {
        if (new_view >= term->grid->offset || new_view <= end)
            new_view = (end + 1) % term->grid->num_rows;
    }

    while (term->grid->rows[new_view] == NULL)
        new_view = (new_view + 1) % term->grid->num_rows;

#if defined(_DEBUG)
    for (int r = 0; r < term->rows; r++)
        assert(term->grid->rows[(new_view + r) % term->grid->num_rows] != NULL);
#endif

    LOG_DBG("scrollback UP: %d -> %d (offset = %d, end = %d, rows = %d)",
            term->grid->view, new_view, term->grid->offset, end, term->grid->num_rows);

    if (new_view == term->grid->view)
        return;

    int diff = -1;
    if (new_view < term->grid->view)
        diff = term->grid->view - new_view;
    else
        diff = (term->grid->num_rows - new_view) + term->grid->view;

    selection_view_up(term, new_view);
    term->grid->view = new_view;

    if (diff >= 0 && diff < term->rows) {
        term_damage_scroll(term, DAMAGE_SCROLL_REVERSE_IN_VIEW, (struct scroll_region){0, term->rows}, diff);
        term_damage_rows_in_view(term, 0, diff - 1);
    } else
        term_damage_view(term);

    render_refresh(term);
}

void
cmd_scrollback_down(struct terminal *term, int rows)
{
    if (term->grid == &term->alt)
        return;

    if (term->mouse_tracking != MOUSE_NONE)
        return;

    if (term->grid->view == term->grid->offset)
        return;

    rows = min(rows, term->rows);
    assert(term->grid->offset >= 0);

    int new_view = (term->grid->view + rows) % term->grid->num_rows;
    assert(new_view >= 0);
    assert(new_view < term->grid->num_rows);

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

#if defined(_DEBUG)
    for (int r = 0; r < term->rows; r++)
        assert(term->grid->rows[(new_view + r) % term->grid->num_rows] != NULL);
#endif

    LOG_DBG("scrollback DOWN: %d -> %d (offset = %d, end = %d, rows = %d)",
            term->grid->view, new_view, term->grid->offset, end, term->grid->num_rows);

    if (new_view == term->grid->view)
        return;

    int diff = -1;
    if (new_view > term->grid->view)
        diff = new_view - term->grid->view;
    else
        diff = (term->grid->num_rows - term->grid->view) + new_view;

    selection_view_down(term, new_view);
    term->grid->view = new_view;

    if (diff >= 0 && diff < term->rows) {
        term_damage_scroll(term, DAMAGE_SCROLL_IN_VIEW, (struct scroll_region){0, term->rows}, diff);
        term_damage_rows_in_view(term, term->rows - diff, term->rows - 1);
    } else
        term_damage_view(term);

    render_refresh(term);
}
