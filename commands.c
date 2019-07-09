#include "commands.h"

#define LOG_MODULE "commands"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "terminal.h"
#include "render.h"
#include "grid.h"

#define max(x, y) ((x) > (y) ? (x) : (y))

void
cmd_scrollback_up(struct terminal *term)
{
    assert(term->grid->offset >= 0);
    int new_view = (term->grid->view + term->grid->num_rows - term->rows) % term->grid->num_rows;

    LOG_WARN("%d" ,new_view);
    assert(new_view >= 0);
    assert(new_view < term->grid->num_rows);

    while (!term->grid->rows[new_view]->initialized)
        new_view = (new_view + 1) % term->grid->num_rows;

    LOG_DBG("scrollback UP: %d -> %d (offset = %d, rows = %d)",
            term->grid->view, new_view, term->grid->offset, term->grid->num_rows);
    term->grid->view = new_view;

    for (int i = 0; i < term->rows; i++)
        grid_row_in_view(term->grid, i)->dirty = true;

    if (term->frame_callback == NULL)
        grid_render(term);
}

void
cmd_scrollback_down(struct terminal *term)
{
    assert(term->grid->offset >= 0);
    int new_view = (term->grid->view + term->rows) % term->grid->num_rows;

    LOG_WARN("%d" ,new_view);
    assert(new_view >= 0);
    assert(new_view < term->grid->num_rows);

    if (!term->grid->rows[new_view]->initialized) {
        while (!term->grid->rows[new_view]->initialized)
            new_view = (new_view + term->grid->num_rows - 1) % term->grid->num_rows;
        new_view = (new_view + term->grid->num_rows - term->rows + 1) % term->grid->num_rows;
    }

    LOG_DBG("scrollback DOWN: %d -> %d (offset = %d, rows = %d)",
            term->grid->view, new_view, term->grid->offset, term->grid->num_rows);
    term->grid->view = new_view;

    for (int i = 0; i < term->rows; i++)
        grid_row_in_view(term->grid, i)->dirty = true;

    if (term->frame_callback == NULL)
        grid_render(term);
}
