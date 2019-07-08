#include "render.h"

#include <string.h>
#include <sys/ioctl.h>

#include <wayland-cursor.h>
#include <xdg-shell.h>

#define LOG_MODULE "render"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "shm.h"
#include "grid.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

cairo_scaled_font_t *
attrs_to_font(struct terminal *term, const struct attributes *attrs)
{
    int idx = attrs->italic << 1 | attrs->bold;
    return term->fonts[idx];
}

struct glyph_sequence {
    cairo_glyph_t glyphs[100000];
    cairo_glyph_t *g;
    int count;

    struct attributes attrs;
    struct rgb foreground;
};

static struct glyph_sequence gseq;

static void
render_cell(struct terminal *term, struct buffer *buf, const struct cell *cell,
            int col, int row)
{
    /* Cursor here? */
    bool has_cursor
        = (!term->hide_cursor &&
           (term->cursor.col == col && term->cursor.row == row));

    double width = term->cell_width;
    double height = term->cell_height;
    double x = col * width;
    double y = row * height;

    const struct rgb *foreground = cell->attrs.have_foreground
        ? &cell->attrs.foreground
        : !term->reverse ? &term->foreground : &term->background;
    const struct rgb *background = cell->attrs.have_background
        ? &cell->attrs.background
        : !term->reverse ? &term->background : &term->foreground;

    /* If *one* is set, we reverse */
    if (has_cursor != cell->attrs.reverse) {
        const struct rgb *swap = foreground;
        foreground = background;
        background = swap;
    }

    /* Background */
    cairo_set_source_rgb(buf->cairo, background->r, background->g, background->b);
    cairo_rectangle(buf->cairo, x, y, width, height);
    cairo_fill(buf->cairo);

    if (cell->c[0] == '\0' || cell->c[0] == ' ')
        return;

    if (cell->attrs.conceal)
        return;

    /*
     * cairo_show_glyphs() apparently works *much* faster when
     * called once with a large array of glyphs, compared to
     * multiple calls with a single glyph.
     *
     * So, collect glyphs until cell attributes change, then we
     * 'flush' (render) the glyphs.
     */

    if (memcmp(&cell->attrs, &gseq.attrs, sizeof(cell->attrs)) != 0 ||
        gseq.count >= sizeof(gseq.glyphs) / sizeof(gseq.glyphs[0]) - 10 ||
        memcmp(&gseq.foreground, foreground, sizeof(*foreground)) != 0)
    {
        if (gseq.count >= sizeof(gseq.glyphs) / sizeof(gseq.glyphs[0]) - 10)
            LOG_WARN("hit glyph limit");

        cairo_set_scaled_font(buf->cairo, attrs_to_font(term, &gseq.attrs));
        cairo_set_source_rgb(
            buf->cairo, gseq.foreground.r, gseq.foreground.g,
            gseq.foreground.b);

        cairo_show_glyphs(buf->cairo, gseq.glyphs, gseq.count);

        gseq.g = gseq.glyphs;
        gseq.count = 0;
        gseq.attrs = cell->attrs;
        gseq.foreground = *foreground;
    }

    int new_glyphs
        = sizeof(gseq.glyphs) / sizeof(gseq.glyphs[0]) - gseq.count;

    cairo_status_t status = cairo_scaled_font_text_to_glyphs(
        attrs_to_font(term, &cell->attrs), x, y + term->fextents.ascent,
        cell->c, strlen(cell->c), &gseq.g, &new_glyphs,
        NULL, NULL, NULL);

    if (status != CAIRO_STATUS_SUCCESS)
        return;

    gseq.g += new_glyphs;
    gseq.count += new_glyphs;
    assert(gseq.count <= sizeof(gseq.glyphs) / sizeof(gseq.glyphs[0]));
}

#if 0
static void
grid_render_update(struct terminal *term, struct buffer *buf, const struct damage *dmg)
{
    LOG_DBG("damage: UPDATE: %d -> %d (offset = %d)",
            (dmg->range.start - term->grid->offset) % term->grid->size,
            (dmg->range.start - term->grid->offset) % term->grid->size + dmg->range.length,
        term->grid->offset);

    int start = dmg->range.start;
    int length = dmg->range.length;

    if (start < term->grid->offset) {
        int end = start + length;
        if (end >= term->grid->offset) {
            start = term->grid->offset;
            length = end - start;
        } else
            return;
    }

    const int cols = term->cols;

    for (int linear_cursor = start,
             row = ((start - term->grid->offset) % term->grid->size) / cols,
             col = ((start - term->grid->offset) % term->grid->size) % cols;
         linear_cursor < start + length;
         linear_cursor++,
             col = col + 1 >= term->cols ? 0 : col + 1,
             row += col == 0 ? 1 : 0)
    {

        assert(row >= 0);
        assert(row < term->rows);
        assert(col >= 0);
        assert(col < term->cols);

        int cell_idx = linear_cursor % term->grid->size;
        if (cell_idx < 0)
            cell_idx += term->grid->size;

        assert(cell_idx >= 0);
        assert(cell_idx < term->rows * term->cols);

        const struct cell *cell = &term->grid->cells[cell_idx];

        /* Cursor here? */
        bool has_cursor
            = (!term->hide_cursor &&
               (term->cursor.linear == linear_cursor - term->grid->offset));

        int x = col * term->cell_width;
        int y = row * term->cell_height;
        int width = term->cell_width;
        int height = term->cell_height;

        struct rgba foreground = cell->attrs.have_foreground
            ? cell->attrs.foreground
            : !term->reverse ? term->foreground : term->background;
        struct rgba background = cell->attrs.have_background
            ? cell->attrs.background
            : !term->reverse ? term->background : term->foreground;

        if (has_cursor) {
            struct rgba swap = foreground;
            foreground = background;
            background = swap;
        }

        if (cell->attrs.reverse) {
            struct rgba swap = foreground;
            foreground = background;
            background = swap;
        }

        /* Background */
        cairo_set_source_rgba(
            buf->cairo, background.r, background.g, background.b, background.a);
        cairo_rectangle(buf->cairo, x, y, width, height);
        cairo_fill(buf->cairo);

        if (cell->c[0] == '\0' || cell->c[0] == ' ')
            continue;

        if (cell->attrs.conceal)
            continue;

        /*
         * cairo_show_glyphs() apparently works *much* faster when
         * called once with a large array of glyphs, compared to
         * multiple calls with a single glyph.
         *
         * So, collect glyphs until cell attributes change, then we
         * 'flush' (render) the glyphs.
         */

        if (memcmp(&cell->attrs, &gseq.attrs, sizeof(cell->attrs)) != 0 ||
            gseq.count >= sizeof(gseq.glyphs) / sizeof(gseq.glyphs[0]) - 10 ||
            memcmp(&gseq.foreground, &foreground, sizeof(foreground)) != 0)
        {
            if (gseq.count >= sizeof(gseq.glyphs) / sizeof(gseq.glyphs[0]) - 10)
                LOG_WARN("hit glyph limit");
            cairo_set_scaled_font(buf->cairo, attrs_to_font(term, &gseq.attrs));
            cairo_set_source_rgba(
                buf->cairo, gseq.foreground.r, gseq.foreground.g,
                gseq.foreground.b, gseq.foreground.a);

            cairo_set_operator(buf->cairo, CAIRO_OPERATOR_OVER);
            cairo_show_glyphs(buf->cairo, gseq.glyphs, gseq.count);

            gseq.g = gseq.glyphs;
            gseq.count = 0;
            gseq.attrs = cell->attrs;
            gseq.foreground = foreground;
        }

        int new_glyphs
            = sizeof(gseq.glyphs) / sizeof(gseq.glyphs[0]) - gseq.count;

        cairo_status_t status = cairo_scaled_font_text_to_glyphs(
            attrs_to_font(term, &cell->attrs), x, y + term->fextents.ascent,
            cell->c, strlen(cell->c), &gseq.g, &new_glyphs,
            NULL, NULL, NULL);

        if (status != CAIRO_STATUS_SUCCESS)
            continue;

        gseq.g += new_glyphs;
        gseq.count += new_glyphs;
        assert(gseq.count <= sizeof(gseq.glyphs) / sizeof(gseq.glyphs[0]));
    }

    wl_surface_damage_buffer(
        term->wl.surface,
        0, ((dmg->range.start - term->grid->offset) / cols) * term->cell_height,
        buf->width, (dmg->range.length + cols - 1) / cols * term->cell_height);
}

static void
grid_render_erase(struct terminal *term, struct buffer *buf, const struct damage *dmg)
{
    LOG_DBG("damage: ERASE: %d -> %d (offset = %d)",
            (dmg->range.start - term->grid->offset) % term->grid->size,
            (dmg->range.start - term->grid->offset) % term->grid->size + dmg->range.length,
            term->grid->offset);

    assert(dmg->range.start >= term->grid->offset);

    const struct rgba *bg = !term->reverse ?
        &term->background : &term->foreground;

    cairo_set_source_rgba(buf->cairo, bg->r, bg->g, bg->b, bg->a);

    const int cols = term->cols;

    int start = (dmg->range.start - term->grid->offset) % term->grid->size;
    int left = dmg->range.length;

    int row = start / cols;
    int col = start % cols;

    /* Partial initial line */
    if (col != 0) {
        int cell_count = min(left, cols - col);

        int x = col * term->cell_width;
        int y = row * term->cell_height;
        int width = cell_count * term->cell_width;
        int height = term->cell_height;

        cairo_rectangle(buf->cairo, x, y, width, height);
        cairo_fill(buf->cairo);
        wl_surface_damage_buffer(term->wl.surface, x, y, width, height);

        start += cell_count;
        left -= cell_count;

        row = start / cols;
        col = start % cols;
    }

    assert(left == 0 || col == 0);

    /* One or more full lines left */
    if (left >= cols) {
        int line_count = left / cols;

        int x = 0;
        int y = row * term->cell_height;
        int width = buf->width;
        int height = line_count * term->cell_height;

        cairo_rectangle(buf->cairo, x, y, width, height);
        cairo_fill(buf->cairo);
        wl_surface_damage_buffer(term->wl.surface, x, y, width, height);

        start += line_count * cols;
        left -= line_count * cols;

        row += line_count;
        col = 0;
    }

    assert(left == 0 || col == 0);
    assert(left < cols);

    /* Partial last line */
    if (left > 0) {
        int x = 0;
        int y = row * term->cell_height;
        int width = left * term->cell_width;
        int height = term->cell_height;

        cairo_rectangle(buf->cairo, x, y, width, height);
        cairo_fill(buf->cairo);
        wl_surface_damage_buffer(term->wl.surface, x, y, width, height);
    }
}
#endif

static void
grid_render_scroll(struct terminal *term, struct buffer *buf,
                   const struct damage *dmg)
{
    int dst_y = (dmg->scroll.region.start + 0) * term->cell_height;
    int src_y = (dmg->scroll.region.start + dmg->scroll.lines) * term->cell_height;
    int width = buf->width;
    int height = (dmg->scroll.region.end - dmg->scroll.region.start - dmg->scroll.lines) * term->cell_height;

    const uint32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

    LOG_DBG("damage: SCROLL: %d-%d by %d lines (dst-y: %d, src-y: %d, "
            "height: %d, stride: %d, mmap-size: %zu)",
            dmg->scroll.region.start, dmg->scroll.region.end,
            dmg->scroll.lines,
            dst_y, src_y, height, stride,
            buf->size);

    if (height > 0) {
        cairo_surface_flush(buf->cairo_surface);
        uint8_t *raw = cairo_image_surface_get_data(buf->cairo_surface);

        memmove(raw + dst_y * stride, raw + src_y * stride, height * stride);
        cairo_surface_mark_dirty(buf->cairo_surface);

        wl_surface_damage_buffer(term->wl.surface, 0, dst_y, width, height);
    }

#if 0
    const int cols = term->cols;

    struct damage erase = {
        .type = DAMAGE_ERASE,
        .range = {
            .start = term->grid->offset + max(dmg->scroll.region.end - dmg->scroll.lines,
                         dmg->scroll.region.start) * cols,
            .length = min(dmg->scroll.region.end - dmg->scroll.region.start,
                          dmg->scroll.lines) * cols,
        },
    };
    grid_render_erase(term, buf, &erase);
#endif
}

static void
grid_render_scroll_reverse(struct terminal *term, struct buffer *buf,
                           const struct damage *dmg)
{
    int src_y = (dmg->scroll.region.start + 0) * term->cell_height;
    int dst_y = (dmg->scroll.region.start + dmg->scroll.lines) * term->cell_height;
    int width = buf->width;
    int height = (dmg->scroll.region.end - dmg->scroll.region.start - dmg->scroll.lines) * term->cell_height;

    const uint32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

    LOG_DBG("damage: SCROLL REVERSE: %d-%d by %d lines (dst-y: %d, src-y: %d, "
            "height: %d, stride: %d, mmap-size: %zu)",
            dmg->scroll.region.start, dmg->scroll.region.end,
            dmg->scroll.lines,
            dst_y, src_y, height, stride,
            buf->size);

    if (height > 0) {
        cairo_surface_flush(buf->cairo_surface);
        uint8_t *raw = cairo_image_surface_get_data(buf->cairo_surface);

        memmove(raw + dst_y * stride, raw + src_y * stride, height * stride);
        cairo_surface_mark_dirty(buf->cairo_surface);

        wl_surface_damage_buffer(term->wl.surface, 0, dst_y, width, height);
    }

#if 0
    const int cols = term->cols;

    struct damage erase = {
        .type = DAMAGE_ERASE,
        .range = {
            .start = term->grid->offset + dmg->scroll.region.start * cols,
            .length = min(dmg->scroll.region.end - dmg->scroll.region.start,
                          dmg->scroll.lines) * cols,
        },
    };
    grid_render_erase(term, buf, &erase);
#endif
}

static void frame_callback(
    void *data, struct wl_callback *wl_callback, uint32_t callback_data);

static const struct wl_callback_listener frame_listener = {
    .done = &frame_callback,
};

void
grid_render(struct terminal *term)
{
    static int last_cursor;

#if 0
    if (tll_length(term->grid->damage) == 0 &&
        tll_length(term->grid->scroll_damage) == 0 &&
        last_cursor == term->grid->offset + term->cursor.linear)
    {
        return;
    }
#endif
    assert(term->width > 0);
    assert(term->height > 0);

    //LOG_WARN("RENDER");

    struct buffer *buf = shm_get_buffer(term->wl.shm, term->width, term->height);
    cairo_set_operator(buf->cairo, CAIRO_OPERATOR_SOURCE);

    static struct buffer *last_buf = NULL;
    if (last_buf != buf || false) {
        if (last_buf != NULL) {
            LOG_WARN("new buffer");

            /* Fill area outside the cell grid with the default background color */
            int rmargin = term->cols * term->cell_width;
            int bmargin = term->rows * term->cell_height;
            int rmargin_width = term->width - rmargin;
            int bmargin_height = term->height - bmargin;

            const struct rgb *bg = !term->reverse ?
                &term->background : &term->foreground;
            cairo_set_source_rgb(buf->cairo, bg->r, bg->g, bg->b);

            cairo_rectangle(buf->cairo, rmargin, 0, rmargin_width, term->height);
            cairo_rectangle(buf->cairo, 0, bmargin, term->width, bmargin_height);
            cairo_fill(buf->cairo);

            wl_surface_damage_buffer(
                term->wl.surface, rmargin, 0, rmargin_width, term->height);
            wl_surface_damage_buffer(
                term->wl.surface, 0, bmargin, term->width, bmargin_height);

            /* Force a full grid refresh */
            term_damage_all(term);
        }
        last_buf = buf;
    }

    bool all_clean = tll_length(term->grid->scroll_damage) == 0;

    tll_foreach(term->grid->scroll_damage, it) {
        switch (it->item.type) {
        case DAMAGE_SCROLL:
            grid_render_scroll(term, buf, &it->item);
            break;

        case DAMAGE_SCROLL_REVERSE:
            grid_render_scroll_reverse(term, buf, &it->item);
            break;

        case DAMAGE_UPDATE:
        case DAMAGE_ERASE:
            assert(false);
            break;
        }

        tll_remove(term->grid->scroll_damage, it);
    }

    gseq.g = gseq.glyphs;
    gseq.count = 0;
#if 0
    tll_foreach(term->grid->damage, it) {
        switch (it->item.type) {
        case DAMAGE_ERASE:  grid_render_erase(term, buf, &it->item); break;
        case DAMAGE_UPDATE: grid_render_update(term, buf, &it->item); break;

        case DAMAGE_SCROLL:
        case DAMAGE_SCROLL_REVERSE:
            assert(false);
            break;
        }

        tll_remove(term->grid->damage, it);
    }
#endif

    for (int r = 0; r < term->rows; r++) {
        struct row *row = grid_row(term->grid, r);

        if (!row->dirty)
            continue;

        //LOG_WARN("rendering line: %d", r);

        for (int col = 0; col < term->cols; col++)
            render_cell(term, buf, &row->cells[col], col, r);

        row->dirty = false;
        all_clean = false;

        wl_surface_damage_buffer(term->wl.surface, 0, r * term->cell_height, term->width, term->cell_height);
    }

    /* TODO: break out to function */
    /* Re-render last cursor cell and current cursor cell */
    /* Make sure previous cursor is refreshed (to avoid "ghost" cursors) */
    int cursor_as_linear
        = (term->grid->offset + term->cursor.row) * term->cols + term->cursor.col;

    if (last_cursor != cursor_as_linear) {
#if 0
        struct damage prev_cursor = {
            .type = DAMAGE_UPDATE,
            .range = {.start = last_cursor, .length = 1},
        };
        grid_render_update(term, buf, &prev_cursor);
#endif
#if 1
        int row = last_cursor / term->cols - term->grid->offset;
        int col = last_cursor % term->cols;
        if (row >= 0 && row < term->rows) {
            render_cell(term, buf, &grid_row(term->grid, row)->cells[col], col, row);
            all_clean = false;

            wl_surface_damage_buffer(
                term->wl.surface, col * term->cell_width, row * term->cell_height,
                term->cell_width, term->cell_height);
        }
        last_cursor = cursor_as_linear;
#endif
    }

    if (all_clean) {
        buf->busy = false;
        return;
    }

#if 0
    struct damage cursor = {
        .type = DAMAGE_UPDATE,
        .range = {.start = term->grid->offset + term->cursor.linear, .length = 1},
    };
    grid_render_update(term, buf, &cursor);
#endif

    render_cell(
        term, buf,
        &grid_row(term->grid, term->cursor.row)->cells[term->cursor.col],
        term->cursor.col, term->cursor.row);

    wl_surface_damage_buffer(
        term->wl.surface,
        term->cursor.col * term->cell_width,
        term->cursor.row * term->cell_height,
        term->cell_width, term->cell_height);

    if (gseq.count > 0) {
        cairo_set_scaled_font(buf->cairo, attrs_to_font(term, &gseq.attrs));
        cairo_set_source_rgb(
            buf->cairo, gseq.foreground.r, gseq.foreground.g,
            gseq.foreground.b);
        cairo_show_glyphs(buf->cairo, gseq.glyphs, gseq.count);
    }

    assert(term->grid->offset >= 0 && term->grid->offset < term->grid->num_rows);
#if 0
    term->grid->offset %= term->grid->size;
    if (term->grid->offset < 0)
        term->grid->offset += term->grid->size;
#endif

    //cairo_surface_flush(buf->cairo_surface);
    wl_surface_attach(term->wl.surface, buf->wl_buf, 0, 0);

    struct wl_callback *cb = wl_surface_frame(term->wl.surface);
    wl_callback_add_listener(cb, &frame_listener, term);
    term->frame_is_scheduled = true;

    wl_surface_commit(term->wl.surface);
}

static void
frame_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct terminal *term = data;

    term->frame_is_scheduled = false;
    wl_callback_destroy(wl_callback);
    grid_render(term);
}

/* Move to terminal.c? */
void
render_resize(struct terminal *term, int width, int height)
{
    if (width == term->width && height == term->height)
        return;

    term->width = width;
    term->height = height;

#if 0
    const size_t old_rows = term->rows;
    const size_t normal_old_size = term->normal.size;
    const size_t alt_old_size = term->alt.size;

    term->cols = term->width / term->cell_width;
    term->rows = term->height / term->cell_height;

    term->normal.size = term->cols * term->rows;
    term->alt.size = term->cols * term->rows;

    term->normal.cells = realloc(
        term->normal.cells,
        term->normal.size * sizeof(term->normal.cells[0]));
    term->alt.cells = realloc(
        term->alt.cells,
        term->alt.size * sizeof(term->alt.cells[0]));

    term->normal.offset
        = (term->normal.offset + term->cols - 1) / term->cols * term->cols;
    term->alt.offset
        = (term->alt.offset + term->cols - 1) / term->cols * term->cols;

    /* TODO: memset */
    for (size_t i = normal_old_size; i < term->normal.size; i++) {
        term->normal.cells[i] = (struct cell){
            .attrs = {.foreground = term->foreground,
                      .background = term->background},
        };
    }

    /* TODO: memset */
    for (size_t i = alt_old_size; i < term->alt.size; i++) {
        term->alt.cells[i] = (struct cell){
            .attrs = {.foreground = term->foreground,
                      .background = term->background},
        };
    }
#endif
    //const int old_cols = term->cols;
    const int old_rows = term->rows;
    const int new_cols = term->width / term->cell_width;
    const int new_rows = term->height / term->cell_height;

    for (int r = 0; r < term->normal.num_rows; r++) {
        free(term->normal.rows[r]->cells);
        free(term->normal.rows[r]);
    }
    free(term->normal.rows);

    for (int r = 0; r < term->alt.num_rows; r++) {
        free(term->alt.rows[r]->cells);
        free(term->alt.rows[r]);
    }
    free(term->alt.rows);

    /* TODO: reflow old content */
    term->normal.num_rows = new_rows;
    term->normal.offset = 0;
    term->alt.num_rows = new_rows;
    term->alt.offset = 0;

    term->normal.rows = malloc(
        term->normal.num_rows * sizeof(term->normal.rows[0]));
    for (int r = 0; r < term->normal.num_rows; r++) {
        struct row *row = malloc(sizeof(*row));
        row->cells = calloc(new_cols,  sizeof(row->cells[0]));
        row->dirty = true;
        term->normal.rows[r] = row;
    }

    term->alt.rows = malloc(term->alt.num_rows * sizeof(term->alt.rows[0]));
    for (int r = 0; r < term->alt.num_rows; r++) {
        struct row *row = malloc(sizeof(*row));
        row->cells = calloc(new_cols, sizeof(row->cells[0]));
        row->dirty = true;
        term->alt.rows[r] = row;
    }

    term->cols = new_cols;
    term->rows = new_rows;

    LOG_INFO("resize: %dx%d, grid: cols=%d, rows=%d",
             term->width, term->height, term->cols, term->rows);

    /* Signal TIOCSWINSZ */
    if (ioctl(term->ptmx, TIOCSWINSZ,
              &(struct winsize){
                  .ws_row = term->rows,
                  .ws_col = term->cols,
                  .ws_xpixel = term->width,
                  .ws_ypixel = term->height}) == -1)
    {
        LOG_ERRNO("TIOCSWINSZ");
    }

    if (term->scroll_region.end == old_rows)
        term->scroll_region.end = term->rows;

#if 0
    term_cursor_to(
        term,
        min(term->cursor.row, term->rows - 1),
        min(term->cursor.col, term->cols - 1));
#endif
    term->cursor.row = term->cursor.col = 0;
    term->grid->cur_row = grid_row(term->grid, 0);

    term_damage_all(term);

    if (!term->frame_is_scheduled)
        grid_render(term);
}

void
render_set_title(struct terminal *term, const char *title)
{
    xdg_toplevel_set_title(term->wl.xdg_toplevel, title);
}

void
render_update_cursor_surface(struct terminal *term)
{
    if (term->wl.pointer.cursor == NULL)
        return;

    //const int scale = backend->monitor->scale;
    const int scale = 1;

    struct wl_cursor_image *image = term->wl.pointer.cursor->images[0];

    wl_surface_set_buffer_scale(term->wl.pointer.surface, scale);

    wl_surface_attach(
        term->wl.pointer.surface, wl_cursor_image_get_buffer(image), 0, 0);

    wl_pointer_set_cursor(
        term->wl.pointer.pointer, term->wl.pointer.serial,
        term->wl.pointer.surface,
        image->hotspot_x / scale, image->hotspot_y / scale);

    wl_surface_damage_buffer(
        term->wl.pointer.surface, 0, 0, INT32_MAX, INT32_MAX);

    wl_surface_commit(term->wl.pointer.surface);
}
