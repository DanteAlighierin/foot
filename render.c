#include "render.h"

#include <string.h>
#include <sys/ioctl.h>

#include <wayland-cursor.h>
#include <xdg-shell.h>

#define LOG_MODULE "render"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "shm.h"

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
    struct rgba foreground;
};

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

    struct glyph_sequence gseq = {.g = gseq.glyphs};

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
            ? cell->attrs.foreground : term->foreground;
        struct rgba background = cell->attrs.have_background
            ? cell->attrs.background : term->background;

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

    if (gseq.count > 0) {
        cairo_set_scaled_font(buf->cairo, attrs_to_font(term, &gseq.attrs));
        cairo_set_source_rgba(
            buf->cairo, gseq.foreground.r, gseq.foreground.g,
            gseq.foreground.b, gseq.foreground.a);
        cairo_set_operator(buf->cairo, CAIRO_OPERATOR_OVER);
        cairo_show_glyphs(buf->cairo, gseq.glyphs, gseq.count);
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

    cairo_set_source_rgba(
        buf->cairo, term->background.r, term->background.g,
        term->background.b, term->background.a);

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

    if (tll_length(term->grid->damage) == 0 &&
        tll_length(term->grid->scroll_damage) == 0 &&
        last_cursor == term->grid->offset + term->cursor.linear)
    {
        return;
    }

    assert(term->width > 0);
    assert(term->height > 0);

    struct buffer *buf = shm_get_buffer(term->wl.shm, term->width, term->height);
    cairo_set_operator(buf->cairo, CAIRO_OPERATOR_SOURCE);

    static struct buffer *last_buf = NULL;
    if (last_buf != buf || false) {
        if (last_buf != NULL) {
            LOG_WARN("new buffer");

            /* Force a full refresh */
            term_damage_all(term);
        }
        last_buf = buf;
    }

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

    /* TODO: break out to function */
    /* Re-render last cursor cell and current cursor cell */
    /* Make sure previous cursor is refreshed (to avoid "ghost" cursors) */
    if (last_cursor != term->cursor.linear) {
        struct damage prev_cursor = {
            .type = DAMAGE_UPDATE,
            .range = {.start = last_cursor, .length = 1},
        };
        grid_render_update(term, buf, &prev_cursor);
    }

    struct damage cursor = {
        .type = DAMAGE_UPDATE,
        .range = {.start = term->grid->offset + term->cursor.linear, .length = 1},
    };
    grid_render_update(term, buf, &cursor);
    last_cursor = term->grid->offset + term->cursor.linear;

    term->grid->offset %= term->grid->size;
    if (term->grid->offset < 0)
        term->grid->offset += term->grid->size;

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

    term_cursor_to(
        term,
        min(term->cursor.row, term->rows - 1),
        min(term->cursor.col, term->cols - 1));

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
