#include "commands.h"

#define LOG_MODULE "commands"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "terminal.h"
#include "render.h"
#include "grid.h"

#define max(x, y) ((x) > (y) ? (x) : (y))
#define min(x, y) ((x) < (y) ? (x) : (y))

void
cmd_scrollback_up(struct terminal *term, int rows)
{
    if (term->grid == &term->alt)
        return;

    rows = min(rows, term->rows);
    assert(term->grid->offset >= 0);

    int new_view = (term->grid->view + term->grid->num_rows - rows) % term->grid->num_rows;
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
            new_view = end + 1;
    } else {
        if (new_view >= term->grid->offset || new_view <= end)
            new_view = end + 1;
    }

#if defined(_DEBUG)
    for (int r = 0; r < term->rows; r++)
        assert(term->grid->rows[(new_view + r) % term->grid->num_rows] != NULL);
#endif

    LOG_DBG("scrollback UP: %d -> %d (offset = %d, end = %d, rows = %d)",
            term->grid->view, new_view, term->grid->offset, end, term->grid->num_rows);

    if (new_view == term->grid->view)
        return;

    term->grid->view = new_view;

    term_damage_view(term);
    render_refresh(term);
}

void
cmd_scrollback_down(struct terminal *term, int rows)
{
    if (term->grid == &term->alt)
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

    term->grid->view = new_view;

    term_damage_view(term);
    render_refresh(term);
}
