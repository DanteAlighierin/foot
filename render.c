#include "render.h"

#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <wayland-cursor.h>
#include <xdg-shell.h>

#define LOG_MODULE "render"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "shm.h"
#include "grid.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

const struct font *
attrs_to_font(struct terminal *term, const struct attributes *attrs)
{
    int idx = attrs->italic << 1 | attrs->bold;
    return &term->fonts[idx];
}

struct glyph_sequence {
    cairo_glyph_t glyphs[100000];
    cairo_glyph_t *g;
    int count;

    struct attributes attrs;
    uint32_t foreground;
};

static struct glyph_sequence gseq;

static void
gseq_flush(struct terminal *term, struct buffer *buf)
{
    struct rgb fg = {
        ((gseq.foreground >> 16) & 0xff) / 255.,
        ((gseq.foreground >>  8) & 0xff) / 255.,
        ((gseq.foreground >>  0) & 0xff) / 255.,
    };

    if (gseq.attrs.dim) {
        fg.r /= 2.;
        fg.g /= 2.;
        fg.b /= 2.;
    }

    cairo_set_scaled_font(buf->cairo, attrs_to_font(term, &gseq.attrs)->font);
    cairo_set_source_rgb(buf->cairo, fg.r, fg.g, fg.b);
    cairo_show_glyphs(buf->cairo, gseq.glyphs, gseq.count);

    gseq.g = gseq.glyphs;
    gseq.count = 0;
}

static void
render_cell(struct terminal *term, struct buffer *buf, const struct cell *cell,
            int col, int row, bool has_cursor)
{
    double width = term->cell_width;
    double height = term->cell_height;
    double x = col * width;
    double y = row * height;

    bool is_selected = false;
    if (term->selection.start.col != -1 && term->selection.end.col != -1) {
        const struct coord *start = &term->selection.start;
        const struct coord *end = &term->selection.end;

        if (end->row < start->row || (end->row == start->row && end->col < start->col)) {
            const struct coord *tmp = start;
            start = end;
            end = tmp;
        }

        assert(start->row <= end->row);

        if (start->row == end->row) {
            is_selected
                = term->grid->view + row == start->row && col >= start->col && col <= end->col;
        } else {
            if (term->grid->view + row == start->row)
                is_selected = col >= start->col;
            else if (term->grid->view + row == end->row)
                is_selected = col <= end->col;
            else
                is_selected = term->grid->view + row >= start->row && term->grid->view + row <= end->row;
        }
    }

    uint32_t _fg = cell->attrs.foreground >> 31
        ? cell->attrs.foreground
        : !term->reverse ? term->foreground : term->background;
    uint32_t _bg = cell->attrs.background >> 31
        ? cell->attrs.background
        : !term->reverse ? term->background : term->foreground;

    /* If *one* is set, we reverse */
    if (has_cursor ^ cell->attrs.reverse ^ is_selected) {
        uint32_t swap = _fg;
        _fg = _bg;
        _bg = swap;
    }

    struct rgb fg = {
        ((_fg >> 16) & 0xff) / 255.,
        ((_fg >>  8) & 0xff) / 255.,
        ((_fg >>  0) & 0xff) / 255.,
    };

    struct rgb bg = {
        ((_bg >> 16) & 0xff) / 255.,
        ((_bg >>  8) & 0xff) / 255.,
        ((_bg >>  0) & 0xff) / 255.,
    };

    if (cell->attrs.dim) {
        fg.r /= 2.;
        fg.g /= 2.;
        fg.b /= 2.;
    }

    /* Background */
    cairo_set_source_rgb(buf->cairo, bg.r, bg.g, bg.b);
    cairo_rectangle(buf->cairo, x, y, width, height);
    cairo_fill(buf->cairo);

    if (cell->c[0] == '\0' || cell->attrs.conceal)
        return;

    /* Underline */
    if (cell->attrs.underline) {
        const struct font *font = attrs_to_font(term, &cell->attrs);
        double baseline = y + term->fextents.height - term->fextents.descent;
        double width = font->underline.thickness;
        double y_under = baseline - font->underline.position - width / 2.;

        cairo_set_source_rgb(buf->cairo, fg.r, fg.g, fg.b);
        cairo_set_line_width(buf->cairo, width);
        cairo_move_to(buf->cairo, x, round(y_under) + 0.5);
        cairo_rel_line_to(buf->cairo, term->cell_width, 0);
        cairo_stroke(buf->cairo);
    }

    if (cell->attrs.strikethrough) {
        const struct font *font = attrs_to_font(term, &cell->attrs);
        double baseline = y + term->fextents.height - term->fextents.descent;
        double width = font->strikeout.thickness;
        double y_strike = baseline - font->strikeout.position - width / 2.;

        cairo_set_source_rgb(buf->cairo, fg.r, fg.g, fg.b);
        cairo_set_line_width(buf->cairo, width);
        cairo_move_to(buf->cairo, x, round(y_strike) + 0.5);
        cairo_rel_line_to(buf->cairo, term->cell_width, 0);
        cairo_stroke(buf->cairo);
    }

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
        gseq.foreground != _fg)
    {
        if (gseq.count >= sizeof(gseq.glyphs) / sizeof(gseq.glyphs[0]) - 10)
            LOG_WARN("hit glyph limit");

        gseq_flush(term, buf);
        gseq.attrs = cell->attrs;
        gseq.foreground = _fg;
    }

    int new_glyphs
        = sizeof(gseq.glyphs) / sizeof(gseq.glyphs[0]) - gseq.count;

    cairo_status_t status = cairo_scaled_font_text_to_glyphs(
        attrs_to_font(term, &cell->attrs)->font, x, y + term->fextents.ascent,
        cell->c, strnlen(cell->c, 4), &gseq.g, &new_glyphs,
        NULL, NULL, NULL);

    if (status != CAIRO_STATUS_SUCCESS)
        return;

    gseq.g += new_glyphs;
    gseq.count += new_glyphs;
    assert(gseq.count <= sizeof(gseq.glyphs) / sizeof(gseq.glyphs[0]));
}

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
}

static void frame_callback(
    void *data, struct wl_callback *wl_callback, uint32_t callback_data);

static const struct wl_callback_listener frame_listener = {
    .done = &frame_callback,
};

void
grid_render(struct terminal *term)
{
    struct timeval start_time;
    gettimeofday(&start_time, NULL);

    static int last_cursor;

    assert(term->width > 0);
    assert(term->height > 0);

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

#if 0
            const struct rgb *bg = !term->reverse ?
                &term->background : &term->foreground;
#else
            uint32_t _bg = !term->reverse ? term->background : term->foreground;
            struct rgb bg = {
                ((_bg >> 16) & 0xff) / 255.,
                ((_bg >>  8) & 0xff) / 255.,
                ((_bg >>  0) & 0xff) / 255.,
            };
#endif
            cairo_set_source_rgb(buf->cairo, bg.r, bg.g, bg.b);

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
        }

        tll_remove(term->grid->scroll_damage, it);
    }

    gseq.g = gseq.glyphs;
    gseq.count = 0;

    for (int r = 0; r < term->rows; r++) {
        struct row *row = grid_row_in_view(term->grid, r);

        if (!row->dirty)
            continue;

        //LOG_WARN("rendering line: %d", r);

        for (int col = 0; col < term->cols; col++)
            render_cell(term, buf, &row->cells[col], col, r, false);

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
        int row = last_cursor / term->cols - term->grid->offset;
        int col = last_cursor % term->cols;
        if (row >= 0 && row < term->rows) {
            render_cell(term, buf, &grid_row_in_view(term->grid, row)->cells[col], col, row, false);
            all_clean = false;

            wl_surface_damage_buffer(
                term->wl.surface, col * term->cell_width, row * term->cell_height,
                term->cell_width, term->cell_height);
        }
        last_cursor = cursor_as_linear;
    }

    if (all_clean) {
        buf->busy = false;
        return;
    }

    bool cursor_is_visible = false;
    int view_end = (term->grid->view + term->rows - 1) % term->grid->num_rows;
    int cursor_row = (term->grid->offset + term->cursor.row) % term->grid->num_rows;
    if (view_end >= term->grid->view) {
        /* Not wrapped */
        if (cursor_row >= term->grid->view && cursor_row <= view_end)
            cursor_is_visible = true;
    } else {
        /* Wrapped */
        if (cursor_row >= term->grid->view || cursor_row <= view_end)
            cursor_is_visible = true;
    }

    if (cursor_is_visible && !term->hide_cursor) {
        render_cell(
            term, buf,
            &grid_row_in_view(term->grid, term->cursor.row)->cells[term->cursor.col],
            term->cursor.col, term->cursor.row, true);

        wl_surface_damage_buffer(
            term->wl.surface,
            term->cursor.col * term->cell_width,
            term->cursor.row * term->cell_height,
            term->cell_width, term->cell_height);
    }

    if (gseq.count > 0)
        gseq_flush(term, buf);

    assert(term->grid->offset >= 0 && term->grid->offset < term->grid->num_rows);
    assert(term->grid->view >= 0 && term->grid->view < term->grid->num_rows);

    cairo_surface_flush(buf->cairo_surface);
    wl_surface_attach(term->wl.surface, buf->wl_buf, 0, 0);

    assert(term->frame_callback == NULL);
    term->frame_callback = wl_surface_frame(term->wl.surface);
    wl_callback_add_listener(term->frame_callback, &frame_listener, term);

    wl_surface_commit(term->wl.surface);

    struct timeval end_time;
    gettimeofday(&end_time, NULL);

    struct timeval render_time;
    timersub(&end_time, &start_time, &render_time);
    LOG_INFO("frame rendered in %lds %ldms",
             render_time.tv_sec, render_time.tv_usec / 1000);
}

static void
frame_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct terminal *term = data;

    assert(term->frame_callback == wl_callback);
    wl_callback_destroy(wl_callback);
    term->frame_callback = NULL;
    grid_render(term);
}

static void
reflow(struct row **new_grid, int new_cols, int new_rows,
       struct row *const *old_grid, int old_cols, int old_rows)
{
    /* TODO: actually reflow */
    for (int r = 0; r < min(new_rows, old_rows); r++) {
        size_t copy_cols = min(new_cols, old_cols);
        size_t clear_cols = new_cols - copy_cols;

        if (old_grid[r] == NULL)
            continue;

        if (new_grid[r] == NULL)
            new_grid[r] = grid_row_alloc(new_cols);

        struct cell *new_cells = new_grid[r]->cells;
        const struct cell *old_cells = old_grid[r]->cells;

        new_grid[r]->dirty = old_grid[r]->dirty;
        memcpy(new_cells, old_cells, copy_cols * sizeof(new_cells[0]));
        memset(&new_cells[copy_cols], 0, clear_cols * sizeof(new_cells[0]));
    }

#if 0
    for (int r = min(new_rows, old_rows); r < new_rows; r++) {
        new_grid[r]->initialized = false;
        new_grid[r]->dirty = false;
        memset(new_grid[r]->cells, 0, new_cols * sizeof(new_grid[r]->cells[0]));
    }
#endif
}

/* Move to terminal.c? */
void
render_resize(struct terminal *term, int width, int height)
{
    if (width == term->width && height == term->height)
        return;

    term->width = width;
    term->height = height;

    const int scrollback_lines = 1000;

    const int old_cols = term->cols;
    const int old_rows = term->rows;
    const int old_normal_grid_rows = term->normal.num_rows;
    const int old_alt_grid_rows = term->alt.num_rows;

    const int new_cols = term->width / term->cell_width;
    const int new_rows = term->height / term->cell_height;
    const int new_normal_grid_rows = new_rows + scrollback_lines;
    const int new_alt_grid_rows = new_rows;

    /* Allocate new 'normal' grid */
    struct row **normal = calloc(new_normal_grid_rows, sizeof(normal[0]));
    for (int r = 0; r < new_rows; r++)
        normal[r] = grid_row_alloc(new_cols);

    /* Allocate new 'alt' grid */
    struct row **alt = calloc(new_alt_grid_rows, sizeof(alt[0]));
    for (int r = 0; r < new_rows; r++)
        alt[r] = grid_row_alloc(new_cols);

    /* Reflow content */
    reflow(normal, new_cols, new_normal_grid_rows,
           term->normal.rows, old_cols, old_normal_grid_rows);
    reflow(alt, new_cols, new_alt_grid_rows,
           term->alt.rows, old_cols, old_alt_grid_rows);

    /* Free old 'normal' grid */
    for (int r = 0; r < term->normal.num_rows; r++)
        grid_row_free(term->normal.rows[r]);
    free(term->normal.rows);

    /* Free old 'alt' grid */
    for (int r = 0; r < term->alt.num_rows; r++)
        grid_row_free(term->alt.rows[r]);
    free(term->alt.rows);

    term->cols = new_cols;
    term->rows = new_rows;

    term->normal.rows = normal;
    term->normal.num_rows = new_normal_grid_rows;
    term->normal.num_cols = new_cols;
    term->alt.rows = alt;
    term->alt.num_rows = new_alt_grid_rows;
    term->alt.num_cols = new_cols;

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

    if (term->scroll_region.start >= term->rows)
        term->scroll_region.start = 0;

    if (term->scroll_region.end >= old_rows)
        term->scroll_region.end = term->rows;

    term->normal.offset %= term->normal.num_rows;
    term->normal.view %= term->normal.num_rows;

    term->alt.offset %= term->alt.num_rows;
    term->alt.view %= term->alt.num_rows;

    term_cursor_to(
        term,
        min(term->cursor.row, term->rows - 1),
        min(term->cursor.col, term->cols - 1));

    term_damage_all(term);
    term_damage_view(term);

    if (term->frame_callback == NULL)
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
