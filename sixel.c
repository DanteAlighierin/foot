#include "sixel.h"

#include <string.h>
#include <limits.h>

#define LOG_MODULE "sixel"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "grid.h"
#include "hsl.h"
#include "render.h"
#include "util.h"
#include "xmalloc.h"
#include "xsnprintf.h"

static size_t count;

void
sixel_fini(struct terminal *term)
{
    free(term->sixel.image.data);
    free(term->sixel.private_palette);
    free(term->sixel.shared_palette);
}

void
sixel_init(struct terminal *term, int p1, int p2, int p3)
{
    /*
     * P1: pixel aspect ratio - unimplemented
     * P2: background color mode
     *  - 0|2: empty pixels use current background color
     *  - 1:   empty pixels remain at their current color (i.e. transparent)
     * P3: horizontal grid size - ignored
     */

    xassert(term->sixel.image.data == NULL);
    xassert(term->sixel.palette_size <= SIXEL_MAX_COLORS);

    term->sixel.state = SIXEL_DECSIXEL;
    term->sixel.pos = (struct coord){0, 0};
    term->sixel.max_non_empty_row_no = -1;
    term->sixel.row_byte_ofs = 0;
    term->sixel.color_idx = 0;
    term->sixel.param = 0;
    term->sixel.param_idx = 0;
    memset(term->sixel.params, 0, sizeof(term->sixel.params));
    term->sixel.transparent_bg = p2 == 1;
    term->sixel.image.data = xmalloc(1 * 6 * sizeof(term->sixel.image.data[0]));
    term->sixel.image.width = 1;
    term->sixel.image.height = 6;

    /* TODO: default palette */

    if (term->sixel.use_private_palette) {
        xassert(term->sixel.private_palette == NULL);
        term->sixel.private_palette = xcalloc(
            term->sixel.palette_size, sizeof(term->sixel.private_palette[0]));
        term->sixel.palette = term->sixel.private_palette;
    } else {
        if (term->sixel.shared_palette == NULL) {
            term->sixel.shared_palette = xcalloc(
                term->sixel.palette_size, sizeof(term->sixel.shared_palette[0]));
        } else {
            /* Shared palette - do *not* reset palette for new sixels */
        }

        term->sixel.palette = term->sixel.shared_palette;
    }

    uint32_t bg = 0;

    switch (term->vt.attrs.bg_src) {
    case COLOR_RGB:
        bg = term->vt.attrs.bg;
        break;

    case COLOR_BASE16:
    case COLOR_BASE256:
        bg = term->colors.table[term->vt.attrs.bg];
        break;

    case COLOR_DEFAULT:
        bg = term->colors.bg;
        break;
    }

    term->sixel.default_bg = term->sixel.transparent_bg
        ? 0x00000000u
        : 0xffu << 24 | bg;

    for (size_t i = 0; i < 1 * 6; i++)
        term->sixel.image.data[i] = term->sixel.default_bg;

    count = 0;
}

void
sixel_destroy(struct sixel *sixel)
{
    if (sixel->pix != NULL)
        pixman_image_unref(sixel->pix);

    free(sixel->data);
    sixel->pix = NULL;
    sixel->data = NULL;
}

void
sixel_destroy_all(struct terminal *term)
{
    tll_foreach(term->normal.sixel_images, it)
        sixel_destroy(&it->item);
    tll_foreach(term->alt.sixel_images, it)
        sixel_destroy(&it->item);
    tll_free(term->normal.sixel_images);
    tll_free(term->alt.sixel_images);
}

static void
sixel_erase(struct terminal *term, struct sixel *sixel)
{
    for (int i = 0; i < sixel->rows; i++) {
        int r = (sixel->pos.row + i) & (term->grid->num_rows - 1);

        struct row *row = term->grid->rows[r];
        if (row == NULL) {
            /* A resize/reflow may cause row to now be unallocated */
            continue;
        }

        row->dirty = true;

        for (int c = sixel->pos.col; c < min(sixel->cols, term->cols); c++)
            row->cells[c].attrs.clean = 0;
    }

    sixel_destroy(sixel);
}

/*
 * Verify the sixels are sorted correctly.
 *
 * The sixels are sorted on their *end* row, in descending order. This
 * invariant means the most recent sixels appear first in the list.
 */
static void
verify_list_order(const struct terminal *term)
{
#if defined(_DEBUG)
    int prev_row = INT_MAX;
    int prev_col = -1;
    int prev_col_count = 0;

    /* To aid debugging */
    size_t idx = 0;

    tll_foreach(term->grid->sixel_images, it) {
        int row = grid_row_abs_to_sb(
            term->grid, term->rows, it->item.pos.row + it->item.rows - 1);
        int col = it->item.pos.col;
        int col_count = it->item.cols;

        xassert(row <= prev_row);

        if (row == prev_row) {
            /* Allowed to be on the same row only if their columns
             * don't overlap */

            xassert(col + col_count <= prev_col ||
                   prev_col + prev_col_count <= col);
        }

        prev_row = row;
        prev_col = col;
        prev_col_count = col_count;
        idx++;
    }
#endif
}

/*
 * Verifies there aren't any sixels that cross the scrollback
 * wrap-around. This invariant means a sixel's absolute row numbers
 * are strictly increasing.
 */
static void
verify_no_wraparound_crossover(const struct terminal *term)
{
#if defined(_DEBUG)
    tll_foreach(term->grid->sixel_images, it) {
        const struct sixel *six = &it->item;

        xassert(six->pos.row >= 0);
        xassert(six->pos.row < term->grid->num_rows);

        int end = (six->pos.row + six->rows - 1) & (term->grid->num_rows - 1);
        xassert(end >= six->pos.row);
    }
#endif
}

/*
 * Verify there aren't any sixels that cross the scrollback end. This
 * invariant means a sixel's rebased row numbers are strictly
 * increasing.
 */
static void
verify_scrollback_consistency(const struct terminal *term)
{
#if defined(_DEBUG)
    tll_foreach(term->grid->sixel_images, it) {
        const struct sixel *six = &it->item;

        int last_row = -1;
        for (int i = 0; i < six->rows; i++) {
            int row_no = grid_row_abs_to_sb(
                term->grid, term->rows, six->pos.row + i);

            if (last_row != -1)
                xassert(last_row < row_no);

            last_row = row_no;
        }
    }
#endif
}

/*
 * Verifies no sixel overlap with any other sixels.
 */
static void
verify_no_overlap(const struct terminal *term)
{
#if defined(_DEBUG)
    tll_foreach(term->grid->sixel_images, it) {
        const struct sixel *six1 = &it->item;

        pixman_region32_t rect1;
        pixman_region32_init_rect(
            &rect1, six1->pos.col, six1->pos.row, six1->cols, six1->rows);

        tll_foreach(term->grid->sixel_images, it2) {
            const struct sixel *six2 = &it2->item;

            if (six1 == six2)
                continue;

            pixman_region32_t rect2;
            pixman_region32_init_rect(
                &rect2, six2->pos.col,
                six2->pos.row, six2->cols, six2->rows);

            pixman_region32_t intersection;
            pixman_region32_init(&intersection);
            pixman_region32_intersect(&intersection, &rect1, &rect2);

            xassert(!pixman_region32_not_empty(&intersection));

            pixman_region32_fini(&intersection);
            pixman_region32_fini(&rect2);
        }

        pixman_region32_fini(&rect1);
    }
#endif
}

static void
verify_sixels(const struct terminal *term)
{
    verify_no_wraparound_crossover(term);
    verify_scrollback_consistency(term);
    verify_no_overlap(term);
    verify_list_order(term);
}

static void
sixel_insert(struct terminal *term, struct sixel sixel)
{
    int end_row = grid_row_abs_to_sb(
        term->grid, term->rows, sixel.pos.row + sixel.rows - 1);

    tll_foreach(term->grid->sixel_images, it) {
        int rebased = grid_row_abs_to_sb(
            term->grid, term->rows, it->item.pos.row + it->item.rows - 1);

        if (rebased < end_row) {
            tll_insert_before(term->grid->sixel_images, it, sixel);
            goto out;
        }
    }

    tll_push_back(term->grid->sixel_images, sixel);

out:
#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    LOG_DBG("sixel list after insertion:");
    tll_foreach(term->grid->sixel_images, it) {
        LOG_DBG("  rows=%d+%d", it->item.pos.row, it->item.rows);
    }
#endif
    verify_sixels(term);
}

void
sixel_scroll_up(struct terminal *term, int rows)
{
    if (likely(tll_length(term->grid->sixel_images) == 0))
        return;

    tll_rforeach(term->grid->sixel_images, it) {
        struct sixel *six = &it->item;

        int six_start = grid_row_abs_to_sb(term->grid, term->rows, six->pos.row);

        if (six_start < rows) {
            sixel_erase(term, six);
            tll_remove(term->grid->sixel_images, it);
        } else {
            /*
             * Unfortunately, we cannot break here.
             *
             * The sixels are sorted on their *end* row. This means
             * there may be a sixel with a top row that will be
             * scrolled out *anywhere* in the list (think of a huuuuge
             * sixel that covers the entire scrollback)
             */
            //break;
        }
    }

    term_update_ascii_printer(term);
    verify_sixels(term);
}

void
sixel_scroll_down(struct terminal *term, int rows)
{
    if (likely(tll_length(term->grid->sixel_images) == 0))
        return;

    xassert(term->grid->num_rows >= rows);

    tll_foreach(term->grid->sixel_images, it) {
        struct sixel *six = &it->item;

        int six_end = grid_row_abs_to_sb(
            term->grid, term->rows, six->pos.row + six->rows - 1);
        if (six_end >= term->grid->num_rows - rows) {
            sixel_erase(term, six);
            tll_remove(term->grid->sixel_images, it);
        } else
            break;
    }

    term_update_ascii_printer(term);
    verify_sixels(term);
}

static void
blend_new_image_over_old(const struct terminal *term,
                         const struct sixel *six, pixman_region32_t *six_rect,
                         int row, int col, pixman_image_t **pix, bool *opaque)
{
    xassert(pix != NULL);
    xassert(opaque != NULL);

    const int six_ofs_x = six->pos.col * term->cell_width;
    const int six_ofs_y = six->pos.row * term->cell_height;
    const int img_ofs_x = col * term->cell_width;
    const int img_ofs_y = row * term->cell_height;
    const int img_width = pixman_image_get_width(*pix);
    const int img_height = pixman_image_get_height(*pix);

    pixman_region32_t pix_rect;
    pixman_region32_init_rect(
        &pix_rect, img_ofs_x, img_ofs_y, img_width, img_height);

    /* Blend the intersection between the old and new images */
    pixman_region32_t intersection;
    pixman_region32_init(&intersection);
    pixman_region32_intersect(&intersection, six_rect, &pix_rect);

    int n_rects = -1;
    pixman_box32_t *boxes = pixman_region32_rectangles(
        &intersection, &n_rects);

    if (n_rects == 0)
        goto out;

    xassert(n_rects == 1);
    pixman_box32_t *box = &boxes[0];

    if (!*opaque) {
        /*
         * New image is transparent - blend on top of the old
         * sixel image.
         */
        pixman_image_composite32(
            PIXMAN_OP_OVER_REVERSE,
            six->pix, NULL, *pix,
            box->x1 - six_ofs_x, box->y1 - six_ofs_y,
            0, 0,
            box->x1 - img_ofs_x, box->y1 - img_ofs_y,
            box->x2 - box->x1, box->y2 - box->y1);
    }

    /*
     * Since the old image is split into sub-tiles on a
     * per-row basis, we need to enlarge the new image and
     * copy the old image if the old image extends beyond the
     * new image.
     *
     * The "bounding" coordinates are either the edges of the
     * old image, or the next cell boundary, whichever comes
     * first.
     */
    int bounding_x = six_ofs_x + six->width > img_ofs_x + img_width
        ? min(
            six_ofs_x + six->width,
            (box->x2 + term->cell_width - 1) / term->cell_width * term->cell_width)
        : box->x2;
    int bounding_y = six_ofs_y + six->height > img_ofs_y + img_height
        ? min(
            six_ofs_y + six->height,
            (box->y2 + term->cell_height - 1) / term->cell_height * term->cell_height)
        : box->y2;

    /* The required size of the new image */
    const int required_width = bounding_x - img_ofs_x;
    const int required_height = bounding_y - img_ofs_y;

    const int new_width = max(img_width, required_width);
    const int new_height = max(img_height, required_height);

    if (new_width <= img_width && new_height <= img_height)
        goto out;

    //LOG_INFO("enlarging: %dx%d -> %dx%d", img_width, img_height, new_width, new_height);

    if (!six->opaque) {
        /* Transparency is viral */
        *opaque = false;
    }

    /* Create a new pixmap */
    int stride = new_width * sizeof(uint32_t);
    uint32_t *new_data = xmalloc(stride * new_height);
    pixman_image_t *pix2 = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8, new_width, new_height, new_data, stride);

#if defined(_DEBUG)
    /* Fill new image with an easy-to-recognize color (green) */
    for (size_t i = 0; i < new_width * new_height; i++)
        new_data[i] = 0xff00ff00;
#endif

    /* Copy the new image, from its old pixmap, to the new pixmap */
    pixman_image_composite32(
        PIXMAN_OP_SRC,
        *pix, NULL, pix2, 0, 0, 0, 0, 0, 0, img_width, img_height);

    /* Copy the bottom tile of the old sixel image into the new pixmap */
    pixman_image_composite32(
        PIXMAN_OP_SRC,
        six->pix, NULL, pix2,
        box->x1 - six_ofs_x, box->y2 - six_ofs_y,
        0, 0,
        box->x1 - img_ofs_x, box->y2 - img_ofs_y,
        bounding_x - box->x1, bounding_y - box->y2);

    /* Copy the right tile of the old sixel image into the new pixmap */
    pixman_image_composite32(
        PIXMAN_OP_SRC,
        six->pix, NULL, pix2,
        box->x2 - six_ofs_x, box->y1 - six_ofs_y,
        0, 0,
        box->x2 - img_ofs_x, box->y1 - img_ofs_y,
        bounding_x - box->x2, bounding_y - box->y1);

    /*
     * Ensure the newly allocated area is initialized.
     *
     * Some of it, or all, will have been initialized above, by the
     * bottom and right tiles from the old sixel image. However, there
     * may be areas in the new image that isn't covered by the old
     * image. These areas need to be made transparent.
     */
    pixman_region32_t uninitialized;
    pixman_region32_init_rects(
        &uninitialized,
        (const pixman_box32_t []){
            /* Extended image area on the right side */
            {img_ofs_x + img_width, img_ofs_y, img_ofs_x + new_width, img_ofs_y + new_height},

            /* Bottom */
            {img_ofs_x, img_ofs_y + img_height, img_ofs_x + new_width, img_ofs_y + new_height}},
        2);

    /* Subtract the old sixel image, since the area(s) covered by the
     * old image has already been copied, and *must* not be
     * overwritten */
    pixman_region32_t diff;
    pixman_region32_init(&diff);
    pixman_region32_subtract(&diff, &uninitialized, six_rect);

    if (pixman_region32_not_empty(&diff)) {
        pixman_image_t *src =
            pixman_image_create_solid_fill(&(pixman_color_t){0});

        int count = -1;
        pixman_box32_t *rects = pixman_region32_rectangles(&diff, &count);

        for (int i = 0; i < count; i++) {
            pixman_image_composite32(
                PIXMAN_OP_SRC,
                src, NULL, pix2,
                0, 0, 0, 0,
                rects[i].x1 - img_ofs_x, rects[i].y1 - img_ofs_y,
                rects[i].x2 - rects[i].x1,
                rects[i].y2 - rects[i].y1);
        }

        pixman_image_unref(src);
        *opaque = false;
    }

    pixman_region32_fini(&diff);
    pixman_region32_fini(&uninitialized);

    /* Use the new pixmap in place of the old one */
    free(pixman_image_get_data(*pix));
    pixman_image_unref(*pix);
    *pix = pix2;

out:
    pixman_region32_fini(&intersection);
    pixman_region32_fini(&pix_rect);
}

static void
sixel_overwrite(struct terminal *term, struct sixel *six,
                int row, int col, int height, int width,
                pixman_image_t **pix, bool *opaque)
{
    pixman_region32_t six_rect;
    pixman_region32_init_rect(
        &six_rect,
        six->pos.col * term->cell_width, six->pos.row * term->cell_height,
        six->width, six->height);

    pixman_region32_t overwrite_rect;
    pixman_region32_init_rect(
        &overwrite_rect,
        col * term->cell_width, row * term->cell_height,
        width * term->cell_width, height * term->cell_height);

#if defined(_DEBUG)
    pixman_region32_t cell_intersection;
    pixman_region32_init(&cell_intersection);
    pixman_region32_intersect(&cell_intersection, &six_rect, &overwrite_rect);
    xassert(pixman_region32_not_empty(&cell_intersection));
    pixman_region32_fini(&cell_intersection);
#endif

    if (pix != NULL)
        blend_new_image_over_old(term, six, &six_rect, row, col, pix, opaque);


    pixman_region32_t diff;
    pixman_region32_init(&diff);
    pixman_region32_subtract(&diff, &six_rect, &overwrite_rect);

    pixman_region32_fini(&six_rect);
    pixman_region32_fini(&overwrite_rect);

    int n_rects = -1;
    pixman_box32_t *boxes = pixman_region32_rectangles(&diff, &n_rects);

    for (int i = 0; i < n_rects; i++) {
        LOG_DBG("box #%d: x1=%d, y1=%d, x2=%d, y2=%d", i,
                boxes[i].x1, boxes[i].y1, boxes[i].x2, boxes[i].y2);

        xassert(boxes[i].x1 % term->cell_width == 0);
        xassert(boxes[i].y1 % term->cell_height == 0);

        /* New image's position, in cells */
        const int new_col = boxes[i].x1 / term->cell_width;
        const int new_row = boxes[i].y1 / term->cell_height;

        xassert(new_row < term->grid->num_rows);

        /* New image's width and height, in pixels */
        const int new_width = boxes[i].x2 - boxes[i].x1;
        const int new_height = boxes[i].y2 - boxes[i].y1;

        uint32_t *new_data = xmalloc(new_width * new_height * sizeof(uint32_t));
        const uint32_t *old_data = six->data;

        /* Pixel offsets into old image backing memory */
        const int x_ofs = boxes[i].x1 - six->pos.col * term->cell_width;
        const int y_ofs = boxes[i].y1 - six->pos.row * term->cell_height;

        /* Copy image data, one row at a time */
        for (size_t j = 0; j < new_height; j++) {
            memcpy(
                &new_data[(0 + j) * new_width],
                &old_data[(y_ofs + j) * six->width + x_ofs],
                new_width * sizeof(uint32_t));
        }

        pixman_image_t *new_pix = pixman_image_create_bits_no_clear(
            PIXMAN_a8r8g8b8,
            new_width, new_height, new_data, new_width * sizeof(uint32_t));

        struct sixel new_six = {
            .data = new_data,
            .pix = new_pix,
            .width = new_width,
            .height = new_height,
            .pos = {.col = new_col, .row = new_row},
            .cols = (new_width + term->cell_width - 1) / term->cell_width,
            .rows = (new_height + term->cell_height - 1) / term->cell_height,
            .opaque = six->opaque,
        };

#if defined(_DEBUG)
        /* Assert we don't cross the scrollback wrap-around */
        const int new_end = new_six.pos.row + new_six.rows - 1;
        xassert(new_end < term->grid->num_rows);
#endif

        sixel_insert(term, new_six);
    }

    pixman_region32_fini(&diff);
}

/* Row numbers are absolute */
static void
_sixel_overwrite_by_rectangle(
    struct terminal *term, int row, int col, int height, int width,
    pixman_image_t **pix, bool *opaque)
{
    verify_sixels(term);

#if defined(_DEBUG)
    pixman_region32_t overwrite_rect;
    pixman_region32_init_rect(&overwrite_rect, col, row, width, height);
#endif

    const int start = row;
    const int end = row + height - 1;

    /* We should never generate scrollback wrapping sixels */
    xassert(end < term->grid->num_rows);

    const int scrollback_rel_start = grid_row_abs_to_sb(
        term->grid, term->rows, start);

    bool UNUSED would_have_breaked = false;

    tll_foreach(term->grid->sixel_images, it) {
        struct sixel *six = &it->item;

        const int six_start = six->pos.row;
        const int six_end = (six_start + six->rows - 1);
        const int six_scrollback_rel_end =
            grid_row_abs_to_sb(term->grid, term->rows, six_end);

        /* We should never generate scrollback wrapping sixels */
        xassert(six_end < term->grid->num_rows);

        if (six_scrollback_rel_end < scrollback_rel_start) {
            /* All remaining sixels are *before* our rectangle */
            would_have_breaked = true;
            break;
        }

#if defined(_DEBUG)
        pixman_region32_t six_rect;
        pixman_region32_init_rect(&six_rect, six->pos.col, six->pos.row, six->cols, six->rows);

        pixman_region32_t intersection;
        pixman_region32_init(&intersection);
        pixman_region32_intersect(&intersection, &six_rect, &overwrite_rect);

        const bool collides = pixman_region32_not_empty(&intersection);
#else
        const bool UNUSED collides = false;
#endif

        if ((start <= six_start && end >= six_start) ||  /* Crosses sixel start boundary */
            (start <= six_end && end >= six_end) ||      /* Crosses sixel end boundary */
            (start >= six_start && end <= six_end))      /* Fully within sixel range */
        {
            const int col_start = six->pos.col;
            const int col_end = six->pos.col + six->cols - 1;

            if ((col <= col_start && col + width - 1 >= col_start) ||
                (col <= col_end && col + width - 1 >= col_end) ||
                (col >= col_start && col + width - 1 <= col_end))
            {
                xassert(!would_have_breaked);

                struct sixel to_be_erased = *six;
                tll_remove(term->grid->sixel_images, it);

                sixel_overwrite(term, &to_be_erased, start, col, height, width,
                                pix, opaque);
                sixel_erase(term, &to_be_erased);
            } else
                xassert(!collides);
        } else
            xassert(!collides);

#if defined(_DEBUG)
        pixman_region32_fini(&intersection);
        pixman_region32_fini(&six_rect);
#endif
    }

#if defined(_DEBUG)
    pixman_region32_fini(&overwrite_rect);
#endif
}

void
sixel_overwrite_by_rectangle(
    struct terminal *term, int row, int col, int height, int width)
{
    if (likely(tll_length(term->grid->sixel_images) == 0))
        return;

    const int start = (term->grid->offset + row) & (term->grid->num_rows - 1);
    const int end = (start + height - 1) & (term->grid->num_rows - 1);
    const bool wraps = end < start;

    if (wraps) {
        int rows_to_wrap_around = term->grid->num_rows - start;
        xassert(height - rows_to_wrap_around > 0);
        _sixel_overwrite_by_rectangle(term, start, col, rows_to_wrap_around, width, NULL, NULL);
        _sixel_overwrite_by_rectangle(term, 0, col, height - rows_to_wrap_around, width, NULL, NULL);
    } else
        _sixel_overwrite_by_rectangle(term, start, col, height, width, NULL, NULL);

    term_update_ascii_printer(term);
}

/* Row numbers are relative to grid offset */
void
sixel_overwrite_by_row(struct terminal *term, int _row, int col, int width)
{
    xassert(col >= 0);

    xassert(_row >= 0);
    xassert(_row < term->rows);
    xassert(col >= 0);
    xassert(col < term->grid->num_cols);

    if (likely(tll_length(term->grid->sixel_images) == 0))
        return;

    if (col + width > term->grid->num_cols)
        width = term->grid->num_cols - col;

    const int row = (term->grid->offset + _row) & (term->grid->num_rows - 1);
    const int scrollback_rel_row = grid_row_abs_to_sb(term->grid, term->rows, row);

    tll_foreach(term->grid->sixel_images, it) {
        struct sixel *six = &it->item;
        const int six_start = six->pos.row;
        const int six_end = (six_start + six->rows - 1) & (term->grid->num_rows - 1);

        /* We should never generate scrollback wrapping sixels */
        xassert(six_end >= six_start);

        const int six_scrollback_rel_end =
            grid_row_abs_to_sb(term->grid, term->rows, six_end);

        if (six_scrollback_rel_end < scrollback_rel_row) {
            /* All remaining sixels are *before* "our" row */
            break;
        }

        if (row >= six_start && row <= six_end) {
            const int col_start = six->pos.col;
            const int col_end = six->pos.col + six->cols - 1;

            if ((col <= col_start && col + width - 1 >= col_start) ||
                (col <= col_end && col + width - 1 >= col_end) ||
                (col >= col_start && col + width - 1 <= col_end))
            {
                struct sixel to_be_erased = *six;
                tll_remove(term->grid->sixel_images, it);

                sixel_overwrite(term, &to_be_erased, row, col, 1, width, NULL, NULL);
                sixel_erase(term, &to_be_erased);
            }
        }
    }

    term_update_ascii_printer(term);
}

void
sixel_overwrite_at_cursor(struct terminal *term, int width)
{
    if (likely(tll_length(term->grid->sixel_images) == 0))
        return;

    sixel_overwrite_by_row(
        term, term->grid->cursor.point.row, term->grid->cursor.point.col, width);
}

void
sixel_cell_size_changed(struct terminal *term)
{
    struct grid *g = term->grid;

    term->grid = &term->normal;
    tll_foreach(term->normal.sixel_images, it) {
        struct sixel *six = &it->item;
        six->rows = (six->height + term->cell_height - 1) / term->cell_height;
        six->cols = (six->width + term->cell_width - 1) / term->cell_width;
    }

    term->grid = &term->alt;
    tll_foreach(term->alt.sixel_images, it) {
        struct sixel *six = &it->item;
        six->rows = (six->height + term->cell_height - 1) / term->cell_height;
        six->cols = (six->width + term->cell_width - 1) / term->cell_width;
    }

    term->grid = g;
}

void
sixel_reflow(struct terminal *term)
{
    struct grid *g = term->grid;

    for (size_t i = 0; i < 2; i++) {
        struct grid *grid = i == 0 ? &term->normal : &term->alt;

        term->grid = grid;

        /* Need the “real” list to be empty from the beginning */
        tll(struct sixel) copy = tll_init();
        tll_foreach(grid->sixel_images, it)
            tll_push_back(copy, it->item);
        tll_free(grid->sixel_images);

        tll_rforeach(copy, it) {
            struct sixel *six = &it->item;
            int start = six->pos.row;
            int end = (start + six->rows - 1) & (grid->num_rows - 1);

            if (end < start) {
                /* Crosses scrollback wrap-around */
                /* TODO: split image */
                sixel_destroy(six);
                continue;
            }

            if (six->rows > grid->num_rows) {
                /* Image too large */
                /* TODO: keep bottom part? */
                sixel_destroy(six);
                continue;
            }

            /* Drop sixels that now cross the current scrollback end
             * border. This is similar to a sixel that have been
             * scrolled out */
            /* TODO: should be possible to optimize this */
            bool sixel_destroyed = false;
            int last_row = -1;

            for (int j = 0; j < six->rows; j++) {
                int row_no = grid_row_abs_to_sb(
                    term->grid, term->rows, six->pos.row + j);
                if (last_row != -1 && last_row >= row_no) {
                    sixel_destroy(six);
                    sixel_destroyed = true;
                    break;
                }

                last_row = row_no;
            }

            if (sixel_destroyed) {
                LOG_WARN("destroyed sixel that now crossed history");
                continue;
            }

            /* Sixels that didn’t overlap may now do so, which isn’t
             * allowed of course */
            _sixel_overwrite_by_rectangle(
                term, six->pos.row, six->pos.col, six->rows, six->cols,
                &it->item.pix, &it->item.opaque);

            if (it->item.data != pixman_image_get_data(it->item.pix)) {
                it->item.data = pixman_image_get_data(it->item.pix);
                it->item.width = pixman_image_get_width(it->item.pix);
                it->item.height = pixman_image_get_height(it->item.pix);
                it->item.cols = (it->item.width + term->cell_width - 1) / term->cell_width;
                it->item.rows = (it->item.height + term->cell_height - 1) / term->cell_height;
            }

            sixel_insert(term, it->item);
        }

        tll_free(copy);
    }

    term->grid = g;
}

void
sixel_unhook(struct terminal *term)
{
    if (term->sixel.image.height > term->sixel.max_non_empty_row_no + 1) {
        LOG_DBG(
            "last row only partially filled, reducing image height: %d -> %d",
            term->sixel.image.height, term->sixel.max_non_empty_row_no + 1);
        term->sixel.image.height = term->sixel.max_non_empty_row_no + 1;
    }

    int pixel_row_idx = 0;
    int pixel_rows_left = term->sixel.image.height;
    const int stride = term->sixel.image.width * sizeof(uint32_t);

    /*
     * When sixel scrolling is enabled (the default), sixels behave
     * pretty much like normal output; the sixel starts at the current
     * cursor position and the cursor is moved to a point after the
     * sixel.
     *
     * Furthermore, if the sixel reaches the bottom of the scrolling
     * region, the terminal content is scrolled.
     *
     * When scrolling is disabled, sixels always start at (0,0), the
     * cursor is not moved at all, and the terminal content never
     * scrolls.
     */

    const bool do_scroll = term->sixel.scrolling;

    /* Number of rows we're allowed to use.
     *
     * When scrolling is enabled, we always allow the entire sixel to
     * be emitted.
     *
     * When disabled, only the number of screen rows may be used. */
    int rows_avail = do_scroll
        ? (term->sixel.image.height + term->cell_height - 1) / term->cell_height
        : term->scroll_region.end;

    /* Initial sixel coordinates */
    int start_row = do_scroll ? term->grid->cursor.point.row : 0;
    const int start_col = do_scroll ? term->grid->cursor.point.col : 0;

    /* Total number of rows needed by image (+ optional newline at the end) */
    const int rows_needed =
        (term->sixel.image.height + term->cell_height - 1) / term->cell_height +
        (term->sixel.cursor_right_of_graphics ? 0 : 1);

    bool free_image_data = true;

    /* We do not allow sixels to cross the scrollback wrap-around, as
     * this makes intersection calculations much more complicated */
    while (pixel_rows_left > 0 &&
           rows_avail > 0 &&
           rows_needed <= term->grid->num_rows)
    {
        const int cur_row = (term->grid->offset + start_row) & (term->grid->num_rows - 1);
        const int rows_left_until_wrap_around = term->grid->num_rows - cur_row;
        const int usable_rows = min(rows_avail, rows_left_until_wrap_around);

        const int pixel_rows_avail = usable_rows * term->cell_height;

        const int width = term->sixel.image.width;
        const int height = min(pixel_rows_left, pixel_rows_avail);

        uint32_t *img_data;
        if (pixel_row_idx == 0 && height == pixel_rows_left) {
            /* Entire image will be emitted as a single chunk - reuse
             * the source buffer */
            img_data = term->sixel.image.data;
            free_image_data = false;
        } else {
            xassert(free_image_data);
            img_data = xmalloc(height * stride);
            memcpy(
                img_data,
                &((uint8_t *)term->sixel.image.data)[pixel_row_idx * stride],
                height * stride);
        }

        struct sixel image = {
            .data = img_data,
            .width = width,
            .height = height,
            .rows = (height + term->cell_height - 1) / term->cell_height,
            .cols = (width + term->cell_width - 1) / term->cell_width,
            .pos = (struct coord){start_col, cur_row},
            .opaque = !term->sixel.transparent_bg,
        };

        xassert(image.rows <= term->grid->num_rows);
        xassert(image.pos.row + image.rows - 1 < term->grid->num_rows);

        LOG_DBG("generating %s %dx%d pixman image at %d-%d",
                image.opaque ? "opaque" : "transparent",
                image.width, image.height,
                image.pos.row, image.pos.row + image.rows);

        image.pix = pixman_image_create_bits_no_clear(
            PIXMAN_a8r8g8b8, image.width, image.height, img_data, stride);

        pixel_row_idx += height;
        pixel_rows_left -= height;
        rows_avail -= image.rows;

        /* Dirty touched cells, and scroll terminal content if necessary */
        for (size_t i = 0; i < image.rows; i++) {
            struct row *row = term->grid->rows[cur_row + i];
            row->dirty = true;

            for (int col = image.pos.col;
                 col < min(image.pos.col + image.cols, term->cols);
                 col++)
            {
                row->cells[col].attrs.clean = 0;
            }

            if (do_scroll) {
                /*
                 * Linefeed, *unless* we're on the very last row of
                 * the final image (not just this chunk) and private
                 * mode 8452 (leave cursor at the right of graphics)
                 * is enabled.
                 */
                if (term->sixel.cursor_right_of_graphics &&
                    rows_avail == 0 &&
                    i >= image.rows - 1)
                {
                    term_cursor_to(
                        term,
                        term->grid->cursor.point.row,
                        min(image.pos.col + image.cols, term->cols - 1));
                } else {
                    term_linefeed(term);
                    term_carriage_return(term);
                }
            }
        }

        _sixel_overwrite_by_rectangle(
            term, image.pos.row, image.pos.col, image.rows, image.cols,
            &image.pix, &image.opaque);

        if (image.data != pixman_image_get_data(image.pix)) {
            image.data = pixman_image_get_data(image.pix);
            image.width = pixman_image_get_width(image.pix);
            image.height = pixman_image_get_height(image.pix);
            image.cols = (image.width + term->cell_width - 1) / term->cell_width;
            image.rows = (image.height + term->cell_height - 1) / term->cell_height;
        }

        sixel_insert(term, image);

        if (do_scroll)
            start_row = term->grid->cursor.point.row;
        else
            start_row -= image.rows;
    }

    if (free_image_data)
        free(term->sixel.image.data);

    term->sixel.image.data = NULL;
    term->sixel.image.width = 0;
    term->sixel.image.height = 0;
    term->sixel.pos = (struct coord){0, 0};

    free(term->sixel.private_palette);
    term->sixel.private_palette = NULL;

    LOG_DBG("you now have %zu sixels in current grid",
            tll_length(term->grid->sixel_images));

    term_update_ascii_printer(term);
    render_refresh(term);
}

static void
resize_horizontally(struct terminal *term, int new_width)
{
    LOG_DBG("resizing image horizontally: %dx(%d) -> %dx(%d)",
            term->sixel.image.width, term->sixel.image.height,
            new_width, term->sixel.image.height);

    if (unlikely(new_width > term->sixel.max_width)) {
        LOG_WARN("maximum image dimensions exceeded, truncating");
        new_width = term->sixel.max_width;
    }

    if (unlikely(term->sixel.image.width == new_width))
        return;

    uint32_t *old_data = term->sixel.image.data;
    const int old_width = term->sixel.image.width;
    const int height = term->sixel.image.height;

    int alloc_height = (height + 6 - 1) / 6 * 6;

    xassert(new_width > 0);
    xassert(alloc_height > 0);

    /* Width (and thus stride) change - need to allocate a new buffer */
    uint32_t *new_data = xmalloc(new_width * alloc_height * sizeof(uint32_t));

    uint32_t bg = term->sixel.default_bg;

    /* Copy old rows, and initialize new columns to background color */
    for (int r = 0; r < height; r++) {
        memcpy(&new_data[r * new_width],
               &old_data[r * old_width],
               old_width * sizeof(uint32_t));

        for (int c = old_width; c < new_width; c++)
            new_data[r * new_width + c] = bg;
    }

    free(old_data);

    term->sixel.image.data = new_data;
    term->sixel.image.width = new_width;
    term->sixel.row_byte_ofs = term->sixel.pos.row * new_width;
}

static bool
resize_vertically(struct terminal *term, int new_height)
{
    LOG_DBG("resizing image vertically: (%d)x%d -> (%d)x%d",
            term->sixel.image.width, term->sixel.image.height,
            term->sixel.image.width, new_height);

    if (unlikely(new_height > term->sixel.max_height)) {
        LOG_WARN("maximum image dimensions reached");
        return false;
    }

    uint32_t *old_data = term->sixel.image.data;
    const int width = term->sixel.image.width;
    const int old_height = term->sixel.image.height;

    int alloc_height = (new_height + 6 - 1) / 6 * 6;

    xassert(width > 0);
    xassert(new_height > 0);

    uint32_t *new_data = realloc(
        old_data, width * alloc_height * sizeof(uint32_t));

    if (new_data == NULL) {
        LOG_ERRNO("failed to reallocate sixel image buffer");
        return false;
    }

    uint32_t bg = term->sixel.default_bg;

    /* Initialize new rows to background color */
    for (int r = old_height; r < new_height; r++) {
        for (int c = 0; c < width; c++)
            new_data[r * width + c] = bg;
    }

    term->sixel.image.data = new_data;
    term->sixel.image.height = new_height;
    return true;
}

static bool
resize(struct terminal *term, int new_width, int new_height)
{
    LOG_DBG("resizing image: %dx%d -> %dx%d",
            term->sixel.image.width, term->sixel.image.height,
            new_width, new_height);

    if (unlikely(new_width > term->sixel.max_width)) {
        LOG_WARN("maximum image width exceeded, truncating");
        new_width = term->sixel.max_width;
    }

    if (unlikely(new_height > term->sixel.max_height)) {
        LOG_WARN("maximum image height exceeded, truncating");
        new_height = term->sixel.max_height;
    }

    uint32_t *old_data = term->sixel.image.data;
    const int old_width = term->sixel.image.width;
    const int old_height = term->sixel.image.height;

    int alloc_new_width = new_width;
    int alloc_new_height = (new_height + 6 - 1) / 6 * 6;
    xassert(alloc_new_height >= new_height);
    xassert(alloc_new_height - new_height < 6);

    uint32_t *new_data = NULL;
    uint32_t bg = term->sixel.default_bg;

    if (new_width == old_width) {
        /* Width (and thus stride) is the same, so we can simply
         * re-alloc the existing buffer */

        new_data = realloc(old_data, alloc_new_width * alloc_new_height * sizeof(uint32_t));
        if (new_data == NULL) {
            LOG_ERRNO("failed to reallocate sixel image buffer");
            return false;
        }

        xassert(new_height > old_height);

    } else {
        /* Width (and thus stride) change - need to allocate a new buffer */
        xassert(new_width > old_width);
        new_data = xmalloc(alloc_new_width * alloc_new_height * sizeof(uint32_t));

        /* Copy old rows, and initialize new columns to background color */
        for (int r = 0; r < min(old_height, new_height); r++) {
            memcpy(&new_data[r * new_width], &old_data[r * old_width], old_width * sizeof(uint32_t));

            for (int c = old_width; c < new_width; c++)
                new_data[r * new_width + c] = bg;
        }
        free(old_data);
    }

    /* Initialize new rows to background color */
    for (int r = old_height; r < new_height; r++) {
        for (int c = 0; c < new_width; c++)
            new_data[r * new_width + c] = bg;
    }

    xassert(new_data != NULL);
    term->sixel.image.data = new_data;
    term->sixel.image.width = new_width;
    term->sixel.image.height = new_height;
    term->sixel.row_byte_ofs = term->sixel.pos.row * new_width;

    return true;
}

static void
sixel_add(struct terminal *term, int col, int width, uint32_t color, uint8_t sixel)
{
    xassert(term->sixel.pos.col < term->sixel.image.width);
    xassert(term->sixel.pos.row < term->sixel.image.height);

    size_t ofs = term->sixel.row_byte_ofs + col;
    uint32_t *data = &term->sixel.image.data[ofs];

    int max_non_empty_row = -1;
    int row = term->sixel.pos.row;

    for (int i = 0; i < 6; i++, sixel >>= 1, data += width) {
        if (sixel & 1) {
            *data = color;
            max_non_empty_row = row + i;
        }
    }

    xassert(sixel == 0);

    term->sixel.max_non_empty_row_no = max(
        term->sixel.max_non_empty_row_no,
        max_non_empty_row);
}

static void
sixel_add_many(struct terminal *term, uint8_t c, unsigned count)
{
    int col = term->sixel.pos.col;
    int width = term->sixel.image.width;

    if (unlikely(col + count - 1 >= width)) {
        resize_horizontally(term, col + count);
        width = term->sixel.image.width;
        count = min(count, width - col);
    }

    uint32_t color = term->sixel.color;
    for (unsigned i = 0; i < count; i++, col++)
        sixel_add(term, col, width, color, c);

    term->sixel.pos.col = col;
}

static void
decsixel(struct terminal *term, uint8_t c)
{
    switch (c) {
    case '"':
        term->sixel.state = SIXEL_DECGRA;
        term->sixel.param = 0;
        term->sixel.param_idx = 0;
        break;

    case '!':
        term->sixel.state = SIXEL_DECGRI;
        term->sixel.param = 0;
        term->sixel.param_idx = 0;
        break;

    case '#':
        term->sixel.state = SIXEL_DECGCI;
        term->sixel.color_idx = 0;
        term->sixel.param = 0;
        term->sixel.param_idx = 0;
        break;

    case '$':
        if (likely(term->sixel.pos.col <= term->sixel.max_width)) {
            /*
             * We set, and keep, ‘col’ outside the image boundary when
             * we’ve reached the maximum image height, to avoid also
             * having to check the row vs image height in the common
             * path in sixel_add().
             */
            term->sixel.pos.col = 0;
        }
        break;

    case '-':
        term->sixel.pos.row += 6;
        term->sixel.pos.col = 0;
        term->sixel.row_byte_ofs += term->sixel.image.width * 6;

        if (term->sixel.pos.row >= term->sixel.image.height) {
            if (!resize_vertically(term, term->sixel.pos.row + 6))
                term->sixel.pos.col = term->sixel.max_width + 1;
        }
        break;

    case '?': case '@': case 'A': case 'B': case 'C': case 'D': case 'E':
    case 'F': case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
    case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R': case 'S':
    case 'T': case 'U': case 'V': case 'W': case 'X': case 'Y': case 'Z':
    case '[': case '\\': case ']': case '^': case '_': case '`': case 'a':
    case 'b': case 'c': case 'd': case 'e': case 'f': case 'g': case 'h':
    case 'i': case 'j': case 'k': case 'l': case 'm': case 'n': case 'o':
    case 'p': case 'q': case 'r': case 's': case 't': case 'u': case 'v':
    case 'w': case 'x': case 'y': case 'z': case '{': case '|': case '}':
    case '~':
        sixel_add_many(term, c - 63, 1);
        break;

    case ' ':
    case '\n':
    case '\r':
        break;

    default:
        LOG_WARN("invalid sixel character: '%c' at idx=%zu", c, count);
        break;
    }
}

static void
decgra(struct terminal *term, uint8_t c)
{
    switch (c) {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        term->sixel.param *= 10;
        term->sixel.param += c - '0';
        break;

    case ';':
        if (term->sixel.param_idx < ALEN(term->sixel.params))
            term->sixel.params[term->sixel.param_idx++] = term->sixel.param;
        term->sixel.param = 0;
        break;

    default: {
        if (term->sixel.param_idx < ALEN(term->sixel.params))
            term->sixel.params[term->sixel.param_idx++] = term->sixel.param;

        int nparams = term->sixel.param_idx;
        unsigned pan = nparams > 0 ? term->sixel.params[0] : 0;
        unsigned pad = nparams > 1 ? term->sixel.params[1] : 0;
        unsigned ph = nparams > 2 ? term->sixel.params[2] : 0;
        unsigned pv = nparams > 3 ? term->sixel.params[3] : 0;

        pan = pan > 0 ? pan : 1;
        pad = pad > 0 ? pad : 1;

        LOG_DBG("pan=%u, pad=%u (aspect ratio = %u), size=%ux%u",
                pan, pad, pan / pad, ph, pv);

        if (ph >= term->sixel.image.height && pv >= term->sixel.image.width &&
            ph <= term->sixel.max_height && pv <= term->sixel.max_width)
        {
            resize(term, ph, pv);

            /* This ensures the sixel’s final image size is *at least*
             * this large */
            term->sixel.max_non_empty_row_no =
                min(pv, term->sixel.image.height) - 1;
        }

        term->sixel.state = SIXEL_DECSIXEL;
        decsixel(term, c);
        break;
    }
    }
}

static void
decgri(struct terminal *term, uint8_t c)
{
    switch (c) {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        term->sixel.param *= 10;
        term->sixel.param += c - '0';
        break;

    case '?': case '@': case 'A': case 'B': case 'C': case 'D': case 'E':
    case 'F': case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
    case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R': case 'S':
    case 'T': case 'U': case 'V': case 'W': case 'X': case 'Y': case 'Z':
    case '[': case '\\': case ']': case '^': case '_': case '`': case 'a':
    case 'b': case 'c': case 'd': case 'e': case 'f': case 'g': case 'h':
    case 'i': case 'j': case 'k': case 'l': case 'm': case 'n': case 'o':
    case 'p': case 'q': case 'r': case 's': case 't': case 'u': case 'v':
    case 'w': case 'x': case 'y': case 'z': case '{': case '|': case '}':
    case '~': {
        unsigned count = term->sixel.param;
        if (likely(count > 0))
            sixel_add_many(term, c - 63, count);
        else if (unlikely(count == 0))
            sixel_add_many(term, c - 63, 1);
        term->sixel.state = SIXEL_DECSIXEL;
        break;
    }

    default:
        term->sixel.state = SIXEL_DECSIXEL;
        sixel_put(term, c);
        break;
    }
}

static void
decgci(struct terminal *term, uint8_t c)
{
    switch (c) {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        term->sixel.param *= 10;
        term->sixel.param += c - '0';
        break;

    case ';':
        if (term->sixel.param_idx < ALEN(term->sixel.params))
            term->sixel.params[term->sixel.param_idx++] = term->sixel.param;
        term->sixel.param = 0;
        break;

    default: {
        if (term->sixel.param_idx < ALEN(term->sixel.params))
            term->sixel.params[term->sixel.param_idx++] = term->sixel.param;

        int nparams = term->sixel.param_idx;

        if (nparams > 0)
            term->sixel.color_idx = min(term->sixel.params[0], term->sixel.palette_size - 1);

        if (nparams > 4) {
            unsigned format = term->sixel.params[1];
            int c1 = term->sixel.params[2];
            int c2 = term->sixel.params[3];
            int c3 = term->sixel.params[4];

            switch (format) {
            case 1: { /* HLS */
                int hue = min(c1, 360);
                int lum = min(c2, 100);
                int sat = min(c3, 100);

                /*
                 * Sixel’s HLS use the following primary color hues:
                 *  blue:  0°
                 *  red:   120°
                 *  green: 240°
                 *
                 * While “standard” HSL uses:
                 *  red:   0°
                 *  green: 120°
                 *  blue:  240°
                 */
                hue = (hue + 240) % 360;

                uint32_t rgb = hsl_to_rgb(hue, sat, lum);

                LOG_DBG("setting palette #%d = HLS %hhu/%hhu/%hhu (0x%06x)",
                        term->sixel.color_idx, hue, lum, sat, rgb);

                term->sixel.palette[term->sixel.color_idx] = 0xffu << 24 | rgb;
                break;
            }

            case 2: {  /* RGB */
                uint8_t r = 255 * min(c1, 100) / 100;
                uint8_t g = 255 * min(c2, 100) / 100;
                uint8_t b = 255 * min(c3, 100) / 100;

                LOG_DBG("setting palette #%d = RGB %hhu/%hhu/%hhu",
                        term->sixel.color_idx, r, g, b);

                term->sixel.palette[term->sixel.color_idx] =
                    0xffu << 24 | r << 16 | g << 8 | b;
                break;
            }
            }
        } else
            term->sixel.color = term->sixel.palette[term->sixel.color_idx];

        term->sixel.state = SIXEL_DECSIXEL;
        decsixel(term, c);
        break;
    }
    }
}

void
sixel_put(struct terminal *term, uint8_t c)
{
    switch (term->sixel.state) {
    case SIXEL_DECSIXEL: decsixel(term, c); break;
    case SIXEL_DECGRA: decgra(term, c); break;
    case SIXEL_DECGRI: decgri(term, c); break;
    case SIXEL_DECGCI: decgci(term, c); break;
    }

    count++;
}

void
sixel_colors_report_current(struct terminal *term)
{
    char reply[24];
    size_t n = xsnprintf(reply, sizeof(reply), "\033[?1;0;%uS", term->sixel.palette_size);
    term_to_slave(term, reply, n);
    LOG_DBG("query response for current color count: %u", term->sixel.palette_size);
}

void
sixel_colors_reset(struct terminal *term)
{
    LOG_DBG("sixel palette size reset to %u", SIXEL_MAX_COLORS);

    free(term->sixel.palette);
    term->sixel.palette = NULL;

    term->sixel.palette_size = SIXEL_MAX_COLORS;
    sixel_colors_report_current(term);
}

void
sixel_colors_set(struct terminal *term, unsigned count)
{
    unsigned new_palette_size = min(max(2, count), SIXEL_MAX_COLORS);
    LOG_DBG("sixel palette size set to %u", new_palette_size);

    free(term->sixel.private_palette);
    free(term->sixel.shared_palette);
    term->sixel.private_palette = NULL;
    term->sixel.shared_palette = NULL;

    term->sixel.palette_size = new_palette_size;
    sixel_colors_report_current(term);
}

void
sixel_colors_report_max(struct terminal *term)
{
    char reply[24];
    size_t n = xsnprintf(reply, sizeof(reply), "\033[?1;0;%uS", SIXEL_MAX_COLORS);
    term_to_slave(term, reply, n);
    LOG_DBG("query response for max color count: %u", SIXEL_MAX_COLORS);
}

void
sixel_geometry_report_current(struct terminal *term)
{
    char reply[64];
    size_t n = xsnprintf(reply, sizeof(reply), "\033[?2;0;%u;%uS",
             min(term->cols * term->cell_width, term->sixel.max_width),
             min(term->rows * term->cell_height, term->sixel.max_height));
    term_to_slave(term, reply, n);

    LOG_DBG("query response for current sixel geometry: %ux%u",
            term->sixel.max_width, term->sixel.max_height);
}

void
sixel_geometry_reset(struct terminal *term)
{
    LOG_DBG("sixel geometry reset to %ux%u", SIXEL_MAX_WIDTH, SIXEL_MAX_HEIGHT);
    term->sixel.max_width = SIXEL_MAX_WIDTH;
    term->sixel.max_height = SIXEL_MAX_HEIGHT;
    sixel_geometry_report_current(term);
}

void
sixel_geometry_set(struct terminal *term, unsigned width, unsigned height)
{
    LOG_DBG("sixel geometry set to %ux%u", width, height);
    term->sixel.max_width = width;
    term->sixel.max_height = height;
    sixel_geometry_report_current(term);
}

void
sixel_geometry_report_max(struct terminal *term)
{
    unsigned max_width = term->sixel.max_width;
    unsigned max_height = term->sixel.max_height;

    char reply[64];
    size_t n = xsnprintf(reply, sizeof(reply), "\033[?2;0;%u;%uS", max_width, max_height);
    term_to_slave(term, reply, n);

    LOG_DBG("query response for max sixel geometry: %ux%u",
            max_width, max_height);
}
