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

static void sixel_put_generic(struct terminal *term, uint8_t c);
static void sixel_put_ar_11(struct terminal *term, uint8_t c);

/* VT330/VT340 Programmer Reference Manual  - Table 2-3 VT340 Default Color Map */
static const uint32_t vt340_default_colors[16] = {
    0xff000000,
    0xff3333cc,
    0xffcc2121,
    0xff33cc33,
    0xffcc33cc,
    0xff33cccc,
    0xffcccc33,
    0xff878787,
    0xff424242,
    0xff545499,
    0xff994242,
    0xff549954,
    0xff995499,
    0xff549999,
    0xff999954,
    0xffcccccc,
};

_Static_assert(sizeof(vt340_default_colors) / sizeof(vt340_default_colors[0]) == 16,
               "wrong number of elements");

void
sixel_fini(struct terminal *term)
{
    free(term->sixel.image.data);
    free(term->sixel.private_palette);
    free(term->sixel.shared_palette);
}

sixel_put
sixel_init(struct terminal *term, int p1, int p2, int p3)
{
    /*
     * P1: pixel aspect ratio
     *  - 0,1   - 2:1
     *  - 2     - 5:1
     *  - 3,4   - 3:1
     *  - 5,6   - 2:1
     *  - 7,8,9 - 1:1
     *
     * P2: background color mode
     *  - 0|2: empty pixels use current background color
     *  - 1:   empty pixels remain at their current color (i.e. transparent)
     * P3: horizontal grid size - ignored
     */

    xassert(term->sixel.image.data == NULL);
    xassert(term->sixel.palette_size <= SIXEL_MAX_COLORS);

    /* Default aspect ratio is 2:1 */
    const int pad = 1;
    const int pan =
        (p1 == 2) ? 5 :
        (p1 == 3 || p1 == 4) ? 3 :
        (p1 == 7 || p1 == 8 || p1 == 9) ? 1 : 2;

    LOG_DBG("initializing sixel with "
            "p1=%d (pan=%d, pad=%d, aspect-ratio=%d:%d), "
            "p2=%d (transparent=%s), "
            "p3=%d (ignored)",
            p1, pan, pad, pan, pad, p2, p2 == 1 ? "yes" : "no", p3);

    term->sixel.state = SIXEL_DECSIXEL;
    term->sixel.pos = (struct coord){0, 0};
    term->sixel.color_idx = 0;
    term->sixel.pan = pan;
    term->sixel.pad = pad;
    term->sixel.param = 0;
    term->sixel.param_idx = 0;
    memset(term->sixel.params, 0, sizeof(term->sixel.params));
    term->sixel.transparent_bg = p2 == 1;
    term->sixel.image.data = NULL;
    term->sixel.image.p = NULL;
    term->sixel.image.width = 0;
    term->sixel.image.height = 0;
    term->sixel.image.alloc_height = 0;
    term->sixel.image.bottom_pixel = 0;

    if (term->sixel.use_private_palette) {
        xassert(term->sixel.private_palette == NULL);
        term->sixel.private_palette = xcalloc(
            term->sixel.palette_size, sizeof(term->sixel.private_palette[0]));

        memcpy(
            term->sixel.private_palette, vt340_default_colors,
            min(sizeof(vt340_default_colors),
                term->sixel.palette_size * sizeof(term->sixel.private_palette[0])));

        term->sixel.palette = term->sixel.private_palette;

    } else {
        if (term->sixel.shared_palette == NULL) {
            term->sixel.shared_palette = xcalloc(
                term->sixel.palette_size, sizeof(term->sixel.shared_palette[0]));

            memcpy(
                term->sixel.shared_palette, vt340_default_colors,
                min(sizeof(vt340_default_colors),
                    term->sixel.palette_size * sizeof(term->sixel.shared_palette[0])));
        } else {
            /* Shared palette - do *not* reset palette for new sixels */
        }

        term->sixel.palette = term->sixel.shared_palette;
    }

    if (term->sixel.transparent_bg)
        term->sixel.default_bg = 0x00000000u;
    else
        term->sixel.default_bg = term->sixel.palette[0];


    count = 0;
    return pan == 1 && pad == 1 ? &sixel_put_ar_11 : &sixel_put_generic;
}

static void
sixel_invalidate_cache(struct sixel *sixel)
{
    if (sixel->scaled.pix != NULL)
        pixman_image_unref(sixel->scaled.pix);

    free(sixel->scaled.data);
    sixel->scaled.pix = NULL;
    sixel->scaled.data = NULL;
    sixel->scaled.width = -1;
    sixel->scaled.height = -1;

    sixel->pix = NULL;
    sixel->width = -1;
    sixel->height = -1;
}

void
sixel_destroy(struct sixel *sixel)
{
    sixel_invalidate_cache(sixel);

    if (sixel->original.pix != NULL)
        pixman_image_unref(sixel->original.pix);

    free(sixel->original.data);
    sixel->original.pix = NULL;
    sixel->original.data = NULL;
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

        for (int c = sixel->pos.col; c < min(sixel->pos.col + sixel->cols, term->cols); c++)
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
    size_t UNUSED idx = 0;

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

    term->bits_affecting_ascii_printer.sixels =
        tll_length(term->grid->sixel_images) > 0;
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

    term->bits_affecting_ascii_printer.sixels =
        tll_length(term->grid->sixel_images) > 0;
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

    /*
     * TODO: handle images being emitted with different cell dimensions
     */

    const int six_ofs_x = six->pos.col * six->cell_width;
    const int six_ofs_y = six->pos.row * six->cell_height;
    const int img_ofs_x = col * six->cell_width;
    const int img_ofs_y = row * six->cell_height;
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
            six->original.pix, NULL, *pix,
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
    int bounding_x = six_ofs_x + six->original.width > img_ofs_x + img_width
        ? min(
            six_ofs_x + six->original.width,
            (box->x2 + six->cell_width - 1) / six->cell_width * six->cell_width)
        : box->x2;
    int bounding_y = six_ofs_y + six->original.height > img_ofs_y + img_height
        ? min(
            six_ofs_y + six->original.height,
            (box->y2 + six->cell_height - 1) / six->cell_height * six->cell_height)
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
        six->original.pix, NULL, pix2,
        box->x1 - six_ofs_x, box->y2 - six_ofs_y,
        0, 0,
        box->x1 - img_ofs_x, box->y2 - img_ofs_y,
        bounding_x - box->x1, bounding_y - box->y2);

    /* Copy the right tile of the old sixel image into the new pixmap */
    pixman_image_composite32(
        PIXMAN_OP_SRC,
        six->original.pix, NULL, pix2,
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
        six->pos.col * six->cell_width, six->pos.row * six->cell_height,
        six->original.width, six->original.height);

    pixman_region32_t overwrite_rect;
    pixman_region32_init_rect(
        &overwrite_rect,
        col * six->cell_width, row * six->cell_height,
        width * six->cell_width, height * six->cell_height);

#if defined(_DEBUG)
    pixman_region32_t cell_intersection;
    pixman_region32_init(&cell_intersection);
    pixman_region32_intersect(&cell_intersection, &six_rect, &overwrite_rect);
    xassert(!pixman_region32_not_empty(&six_rect) ||
            pixman_region32_not_empty(&cell_intersection));
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

        xassert(boxes[i].x1 % six->cell_width == 0);
        xassert(boxes[i].y1 % six->cell_height == 0);

        /* New image's position, in cells */
        const int new_col = boxes[i].x1 / six->cell_width;
        const int new_row = boxes[i].y1 / six->cell_height;

        xassert(new_row < term->grid->num_rows);

        /* New image's width and height, in pixels */
        const int new_width = boxes[i].x2 - boxes[i].x1;
        const int new_height = boxes[i].y2 - boxes[i].y1;

        uint32_t *new_data = xmalloc(new_width * new_height * sizeof(uint32_t));
        const uint32_t *old_data = six->original.data;

        /* Pixel offsets into old image backing memory */
        const int x_ofs = boxes[i].x1 - six->pos.col * six->cell_width;
        const int y_ofs = boxes[i].y1 - six->pos.row * six->cell_height;

        /* Copy image data, one row at a time */
        for (size_t j = 0; j < new_height; j++) {
            memcpy(
                &new_data[(0 + j) * new_width],
                &old_data[(y_ofs + j) * six->original.width + x_ofs],
                new_width * sizeof(uint32_t));
        }

        pixman_image_t *new_pix = pixman_image_create_bits_no_clear(
            PIXMAN_a8r8g8b8,
            new_width, new_height, new_data, new_width * sizeof(uint32_t));

        struct sixel new_six = {
            .pix = NULL,
            .width = -1,
            .height = -1,
            .pos = {.col = new_col, .row = new_row},
            .cols = (new_width + six->cell_width - 1) / six->cell_width,
            .rows = (new_height + six->cell_height - 1) / six->cell_height,
            .opaque = six->opaque,
            .cell_width = six->cell_width,
            .cell_height = six->cell_height,
            .original = {
                .data = new_data,
                .pix = new_pix,
                .width = new_width,
                .height = new_height,
            },
            .scaled = {
                .data = NULL,
                .pix = NULL,
                .width = -1,
                .height = -1,
            },
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

    term->bits_affecting_ascii_printer.sixels =
        tll_length(term->grid->sixel_images) > 0;
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

    term->bits_affecting_ascii_printer.sixels =
        tll_length(term->grid->sixel_images) > 0;
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
    tll_foreach(term->normal.sixel_images, it)
        sixel_invalidate_cache(&it->item);

    tll_foreach(term->alt.sixel_images, it)
        sixel_invalidate_cache(&it->item);
}

void
sixel_sync_cache(const struct terminal *term, struct sixel *six)
{
    if (six->pix != NULL) {
#if defined(_DEBUG)
        if (six->cell_width == term->cell_width &&
            six->cell_height == term->cell_height)
        {
            xassert(six->pix == six->original.pix);
            xassert(six->width == six->original.width);
            xassert(six->height == six->original.height);

            xassert(six->scaled.data == NULL);
            xassert(six->scaled.pix == NULL);
            xassert(six->scaled.width < 0);
            xassert(six->scaled.height < 0);
        } else {
            xassert(six->pix == six->scaled.pix);
            xassert(six->width == six->scaled.width);
            xassert(six->height == six->scaled.height);

            xassert(six->scaled.data != NULL);
            xassert(six->scaled.pix != NULL);

            /* TODO: check ratio */
            xassert(six->scaled.width >= 0);
            xassert(six->scaled.height >= 0);
        }
#endif
        return;
    }

    /* Cache should be invalid */
    xassert(six->scaled.data == NULL);
    xassert(six->scaled.pix == NULL);
    xassert(six->scaled.width < 0);
    xassert(six->scaled.height < 0);

    if (six->cell_width == term->cell_width &&
        six->cell_height == term->cell_height)
    {
        six->pix = six->original.pix;
        six->width = six->original.width;
        six->height = six->original.height;
    } else {
        const double width_ratio = (double)term->cell_width / six->cell_width;
        const double height_ratio = (double)term->cell_height / six->cell_height;

        struct pixman_f_transform scale;
        pixman_f_transform_init_scale(
            &scale, 1. / width_ratio, 1. / height_ratio);

        struct pixman_transform _scale;
        pixman_transform_from_pixman_f_transform(&_scale, &scale);
        pixman_image_set_transform(six->original.pix, &_scale);
        pixman_image_set_filter(six->original.pix, PIXMAN_FILTER_BILINEAR, NULL, 0);

        int scaled_width = (double)six->original.width * width_ratio;
        int scaled_height = (double)six->original.height * height_ratio;
        int scaled_stride = scaled_width * sizeof(uint32_t);

        LOG_DBG("scaling sixel: %dx%d -> %dx%d",
                six->original.width, six->original.height,
                scaled_width, scaled_height);

        uint8_t *scaled_data = xmalloc(scaled_height * scaled_stride);
        pixman_image_t *scaled_pix = pixman_image_create_bits_no_clear(
            PIXMAN_a8r8g8b8, scaled_width, scaled_height,
            (uint32_t *)scaled_data, scaled_stride);

        pixman_image_composite32(
            PIXMAN_OP_SRC, six->original.pix, NULL, scaled_pix, 0, 0, 0, 0,
            0, 0, scaled_width, scaled_height);

        pixman_image_set_transform(six->original.pix, NULL);

        six->scaled.data = scaled_data;
        six->scaled.pix = six->pix = scaled_pix;
        six->scaled.width = six->width = scaled_width;
        six->scaled.height = six->height = scaled_height;
    }
}

void
sixel_reflow_grid(struct terminal *term, struct grid *grid)
{
    /* Meh - the sixel functions we call use term->grid... */
    struct grid *active_grid = term->grid;
    term->grid = grid;

    /* Need the "real" list to be empty from the beginning */
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

        /* Sixels that didn't overlap may now do so, which isn't
         * allowed of course */
        _sixel_overwrite_by_rectangle(
            term, six->pos.row, six->pos.col, six->rows, six->cols,
            &it->item.original.pix, &it->item.opaque);

        if (it->item.original.data != pixman_image_get_data(it->item.original.pix)) {
            it->item.original.data = pixman_image_get_data(it->item.original.pix);
            it->item.original.width = pixman_image_get_width(it->item.original.pix);
            it->item.original.height = pixman_image_get_height(it->item.original.pix);
            it->item.cols = (it->item.original.width + it->item.cell_width - 1) / it->item.cell_width;
            it->item.rows = (it->item.original.height + it->item.cell_height - 1) / it->item.cell_height;
            sixel_invalidate_cache(&it->item);
        }

        sixel_insert(term, it->item);
    }

    tll_free(copy);
    term->grid = active_grid;
}

void
sixel_reflow(struct terminal *term)
{
    for (size_t i = 0; i < 2; i++) {
        struct grid *grid = i == 0 ? &term->normal : &term->alt;
        sixel_reflow_grid(term, grid);
    }
}

void
sixel_unhook(struct terminal *term)
{
    if (term->sixel.pos.row < term->sixel.image.height &&
        term->sixel.pos.row + 6 * term->sixel.pan >= term->sixel.image.height)
    {
        /*
         * Handle case where image has had its size set by raster
         * attributes, and then one or more sixels were printed on the
         * last row of the RA area.
         *
         * In this case, the image height may not be a multiple of
         * 6*pan. But the printed sixels may still be outside the RA
         * area. In this case, using the size from the RA would
         * truncate the image.
         *
         * So, extend the image to a multiple of 6*pan.
         *
         * If this is a transparent image, the image may get trimmed
         * below (most likely back the size set by RA).
         */
        term->sixel.image.height = term->sixel.image.alloc_height;
    }

    /* Strip trailing fully transparent rows, *unless* we *ended* with
     * a trailing GNL, in which case we do *not* want to strip all 6
     * pixel rows */
    if (term->sixel.pos.col > 0) {
        const int bits = sizeof(term->sixel.image.bottom_pixel) * 8;
        const int leading_zeroes = term->sixel.image.bottom_pixel == 0
            ? bits
            : __builtin_clz(term->sixel.image.bottom_pixel);
        const int rows_to_trim = leading_zeroes + 6 - bits;

        LOG_DBG("bottom-pixel: 0x%02x, bits=%d, leading-zeroes=%d, "
                "rows-to-trim=%d*%d", term->sixel.image.bottom_pixel,
                bits, leading_zeroes, rows_to_trim, term->sixel.pan);

        /*
         * If the current graphical cursor position is at the last row
         * of the image, *and* the image is transparent (P2=1), trim
         * the entire image.
         *
         * If the image is not transparent, then we can't trim the RA
         * region (it is supposed to "erase", with the current
         * background color.)
         *
         * We *do* "trim" transparent rows from the graphical cursor
         * position, as this affects the positioning of the text
         * cursor.
         *
         * See https://raw.githubusercontent.com/hackerb9/vt340test/main/sixeltests/p2effect.sh
         */
        if (term->sixel.pos.row + 6 * term->sixel.pan >= term->sixel.image.alloc_height) {
            LOG_DBG("trimming image");
            const int trimmed_height =
                term->sixel.image.alloc_height - rows_to_trim * term->sixel.pan;

            if (term->sixel.transparent_bg) {
                /* Image is transparent - trim as much as possible */
                term->sixel.image.height = trimmed_height;
            } else  {
                /* Image is opaque. We can't trim anything "inside"
                   the RA region */
                if (trimmed_height > term->sixel.image.height) {
                    /* There are non-empty pixels *outside* the RA
                       region - trim up to that point */
                    term->sixel.image.height = trimmed_height;
                }
            }
        } else {
            LOG_DBG("only adjusting cursor position");
        }

        term->sixel.pos.row += 6 * term->sixel.pan;
        term->sixel.pos.row -= rows_to_trim * term->sixel.pan;
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

    /* Total number of rows needed by image */
    const int rows_needed =
        (term->sixel.image.height + term->cell_height - 1) / term->cell_height;

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
            .pix = NULL,
            .width = -1,
            .height = -1,
            .rows = (height + term->cell_height - 1) / term->cell_height,
            .cols = (width + term->cell_width - 1) / term->cell_width,
            .pos = (struct coord){start_col, cur_row},
            .opaque = !term->sixel.transparent_bg,
            .cell_width = term->cell_width,
            .cell_height = term->cell_height,
            .original = {
                .data = img_data,
                .pix = NULL,
                .width = width,
                .height = height,
            },
            .scaled = {
                .data = NULL,
                .pix = NULL,
                .width = -1,
                .height = -1,
            },
        };

        xassert(image.rows <= term->grid->num_rows);
        xassert(image.pos.row + image.rows - 1 < term->grid->num_rows);

        LOG_DBG("generating %s %dx%d pixman image at %d-%d",
                image.opaque ? "opaque" : "transparent",
                image.original.width, image.original.height,
                image.pos.row, image.pos.row + image.rows);

        image.original.pix = pixman_image_create_bits_no_clear(
            PIXMAN_a8r8g8b8, image.original.width, image.original.height,
            img_data, stride);

        pixel_row_idx += height;
        pixel_rows_left -= height;
        rows_avail -= image.rows;

        if (do_scroll) {
            /*
             * Linefeeds - always one less than the number of rows
             * occupied by the image.
             *
             * Unless this is *not* the last chunk. In that case,
             * linefeed past the chunk, so that the next chunk
             * "starts" at a "new" row.
             */
            const int linefeed_count = rows_avail == 0
                ? max(0, image.rows - 1)
                : image.rows;

            xassert(rows_avail == 0 ||
                    image.original.height % term->cell_height == 0);

            for (size_t i = 0; i < linefeed_count; i++)
                term_linefeed(term);

            /* Position text cursor if this is the last image chunk */
            if (rows_avail == 0) {
                int row = term->grid->cursor.point.row;

                /*
                 * Position the text cursor based on the text row
                 * touched by the last sixel
                 */
                const int pixel_rows = pixel_rows_left > 0
                    ? image.original.height
                    : term->sixel.pos.row;
                const int term_rows =
                    (pixel_rows + term->cell_height - 1) / term->cell_height;

                xassert(term_rows <= image.rows);

                row -= (image.rows - term_rows);

                term_cursor_to(
                    term,
                    max(0, row),
                    (term->sixel.cursor_right_of_graphics
                     ? min(image.pos.col + image.cols, term->cols - 1)
                     : image.pos.col));
            }

            term->sixel.pos.row -= image.original.height;
        }

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

        }

        _sixel_overwrite_by_rectangle(
            term, image.pos.row, image.pos.col, image.rows, image.cols,
            &image.original.pix, &image.opaque);

        if (image.original.data != pixman_image_get_data(image.original.pix)) {
            image.original.data = pixman_image_get_data(image.original.pix);
            image.original.width = pixman_image_get_width(image.original.pix);
            image.original.height = pixman_image_get_height(image.original.pix);
            image.cols = (image.original.width + image.cell_width - 1) / image.cell_width;
            image.rows = (image.original.height + image.cell_height - 1) / image.cell_height;
            sixel_invalidate_cache(&image);
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
    term->sixel.image.p = NULL;
    term->sixel.image.width = 0;
    term->sixel.image.height = 0;
    term->sixel.pos = (struct coord){0, 0};

    free(term->sixel.private_palette);
    term->sixel.private_palette = NULL;

    LOG_DBG("you now have %zu sixels in current grid",
            tll_length(term->grid->sixel_images));


    term->bits_affecting_ascii_printer.sixels =
        tll_length(term->grid->sixel_images) > 0;
    term_update_ascii_printer(term);
    render_refresh(term);
}

static void ALWAYS_INLINE inline
memset_u32(uint32_t *data, uint32_t value, size_t count)
{
    static_assert(sizeof(wchar_t) == 4, "wchar_t is not 4 bytes");
    wmemset((wchar_t *)data, (wchar_t)value, count);
}

static void
resize_horizontally(struct terminal *term, int new_width_mutable)
{
    if (unlikely(new_width_mutable > term->sixel.max_width)) {
        LOG_WARN("maximum image dimensions exceeded, truncating");
        new_width_mutable = term->sixel.max_width;
    }

    if (unlikely(term->sixel.image.width >= new_width_mutable))
        return;

    const int sixel_row_height = 6 * term->sixel.pan;

    uint32_t *old_data = term->sixel.image.data;
    const int old_width = term->sixel.image.width;
    const int new_width = new_width_mutable;

    int height;
    if (unlikely(term->sixel.image.height == 0)) {
        /* Lazy initialize height on first printed sixel */
        xassert(old_width == 0);
        term->sixel.image.height = height = sixel_row_height;
        term->sixel.image.alloc_height = sixel_row_height;
    } else
        height = term->sixel.image.height;

    LOG_DBG("resizing image horizontally: %dx(%d) -> %dx(%d)",
            term->sixel.image.width, term->sixel.image.height,
            new_width, height);

    int alloc_height = (height + sixel_row_height - 1) / sixel_row_height * sixel_row_height;

    xassert(new_width >= old_width);
    xassert(new_width > 0);
    xassert(alloc_height > 0);

    /* Width (and thus stride) change - need to allocate a new buffer */
    uint32_t *new_data = xmalloc(new_width * alloc_height * sizeof(uint32_t));

    uint32_t bg = term->sixel.default_bg;

    /* Copy old rows, and initialize new columns to background color */
    const uint32_t *end = &new_data[alloc_height * new_width];
    for (uint32_t *n = new_data, *o = old_data;
         n < end;
         n += new_width, o += old_width)
    {
        memcpy(n, o, old_width * sizeof(uint32_t));
        memset_u32(&n[old_width], bg, new_width - old_width);
    }

    free(old_data);

    term->sixel.image.data = new_data;
    term->sixel.image.width = new_width;

    const int ofs = term->sixel.pos.row * new_width + term->sixel.pos.col;
    term->sixel.image.p = &term->sixel.image.data[ofs];
}

static bool
resize_vertically(struct terminal *term, const int new_height)
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
    const int sixel_row_height = 6 * term->sixel.pan;

    int alloc_height = (new_height + sixel_row_height - 1) / sixel_row_height * sixel_row_height;

    xassert(new_height > 0);

    if (unlikely(width == 0)) {
        xassert(term->sixel.image.data == NULL);
        term->sixel.image.height = new_height;
        term->sixel.image.alloc_height = alloc_height;
        return true;
    }

    uint32_t *new_data = realloc(
        old_data, width * alloc_height * sizeof(uint32_t));

    if (new_data == NULL) {
        LOG_ERRNO("failed to reallocate sixel image buffer");
        return false;
    }

    const uint32_t bg = term->sixel.default_bg;

    memset_u32(&new_data[old_height * width],
               bg,
               (alloc_height - old_height) * width);

    term->sixel.image.height = new_height;
    term->sixel.image.alloc_height = alloc_height;

    const int ofs =
        term->sixel.pos.row * term->sixel.image.width + term->sixel.pos.col;

    term->sixel.image.data = new_data;
    term->sixel.image.p = &term->sixel.image.data[ofs];

    return true;
}

static bool
resize(struct terminal *term, int new_width_mutable, int new_height_mutable)
{
    LOG_DBG("resizing image: %dx%d -> %dx%d",
            term->sixel.image.width, term->sixel.image.height,
            new_width_mutable, new_height_mutable);

    if (unlikely(new_width_mutable > term->sixel.max_width)) {
        LOG_WARN("maximum image width exceeded, truncating");
        new_width_mutable = term->sixel.max_width;
    }

    if (unlikely(new_height_mutable > term->sixel.max_height)) {
        LOG_WARN("maximum image height exceeded, truncating");
        new_height_mutable = term->sixel.max_height;
    }


    uint32_t *old_data = term->sixel.image.data;
    const int old_width = term->sixel.image.width;
    const int old_height = term->sixel.image.height;
    const int new_width = new_width_mutable;
    const int new_height = new_height_mutable;

    if (unlikely(old_width == new_width && old_height == new_height))
        return true;

    const int sixel_row_height = 6 * term->sixel.pan;
    const int alloc_new_height =
        (new_height + sixel_row_height - 1) / sixel_row_height * sixel_row_height;

    xassert(alloc_new_height >= new_height);
    xassert(alloc_new_height - new_height < sixel_row_height);

    uint32_t *new_data = NULL;
    const uint32_t bg = term->sixel.default_bg;

    /*
     * If the image is resized horizontally, or if it's opaque, we
     * need to explicitly initialize the "new" pixels.
     *
     * When the image is *not* resized horizontally, we simply do a
     * realloc(). In this case, there's no need to manually copy the
     * old pixels. We do however need to initialize the new pixels
     * since realloc() returns uninitialized memory.
     *
     * When the image *is* resized horizontally, we need to allocate
     * new memory (when the width changes, the stride changes, and
     * thus we cannot simply realloc())
     *
     * If the default background is transparent, the new pixels need
     * to be initialized to 0x0. We do this by using calloc().
     *
     * If the default background is opaque, then we need to manually
     * initialize the new pixels.
     */
    const bool initialize_bg =
        !term->sixel.transparent_bg || new_width == old_width;

    if (new_width == old_width) {
        /* Width (and thus stride) is the same, so we can simply
         * re-alloc the existing buffer */

        new_data = realloc(old_data, new_width * alloc_new_height * sizeof(uint32_t));
        if (new_data == NULL) {
            LOG_ERRNO("failed to reallocate sixel image buffer");
            return false;
        }

        xassert(new_height > old_height);

    } else {
        /* Width (and thus stride) change - need to allocate a new buffer */
        xassert(new_width > old_width);
        const size_t pixels = new_width * alloc_new_height;

        new_data = !initialize_bg
            ? xcalloc(pixels, sizeof(uint32_t))
            : xmalloc(pixels * sizeof(uint32_t));

        /* Copy old rows, and initialize new columns to background color */
        const int row_copy_count = min(old_height, alloc_new_height);
        const uint32_t *end = &new_data[row_copy_count * new_width];

        for (uint32_t *n = new_data, *o = old_data;
             n < end;
             n += new_width, o += old_width)
        {
            memcpy(n, o, old_width * sizeof(uint32_t));
            memset_u32(&n[old_width], bg, new_width - old_width);
        }
        free(old_data);
    }

    if (initialize_bg) {
        memset_u32(&new_data[old_height * new_width],
                   bg,
                   (alloc_new_height - old_height) * new_width);
    }

    xassert(new_data != NULL);
    term->sixel.image.data = new_data;
    term->sixel.image.width = new_width;
    term->sixel.image.height = new_height;
    term->sixel.image.alloc_height = alloc_new_height;
    term->sixel.image.p = &term->sixel.image.data[term->sixel.pos.row * new_width + term->sixel.pos.col];

    return true;
}

static void
sixel_add_generic(struct terminal *term, uint32_t *data, int stride, uint32_t color,
                  uint8_t sixel)
{
    const int pan = term->sixel.pan;

    for (int i = 0; i < 6; i++, sixel >>= 1) {
        if (sixel & 1) {
            for (int r = 0; r < pan; r++, data += stride)
                *data = color;
        } else
            data += stride * pan;
    }

    xassert(sixel == 0);
}

static void ALWAYS_INLINE inline
sixel_add_ar_11(struct terminal *term, uint32_t *data, int stride, uint32_t color,
                uint8_t sixel)
{
    xassert(term->sixel.pan == 1);

    if (sixel & 0x01)
        *data = color;
    data += stride;
    if (sixel & 0x02)
        *data = color;
    data += stride;
    if (sixel & 0x04)
        *data = color;
    data += stride;
    if (sixel & 0x08)
        *data = color;
    data += stride;
    if (sixel & 0x10)
        *data = color;
    data += stride;
    if (sixel & 0x20)
        *data = color;
}

static void
sixel_add_many_generic(struct terminal *term, uint8_t c, unsigned count)
{
    int col = term->sixel.pos.col;
    int width = term->sixel.image.width;

    count *= term->sixel.pad;

    if (unlikely(col + count - 1 >= width)) {
        resize_horizontally(term, col + count);
        width = term->sixel.image.width;
        count = min(count, max(width - col, 0));

        if (unlikely(count == 0))
            return;
    }

    uint32_t color = term->sixel.color;
    uint32_t *data = term->sixel.image.p;
    uint32_t *end = data + count;

    term->sixel.pos.col = col + count;
    term->sixel.image.p = end;
    term->sixel.image.bottom_pixel |= c;

    for (; data < end; data++)
        sixel_add_generic(term, data, width, color, c);

}

static void ALWAYS_INLINE inline
sixel_add_one_ar_11(struct terminal *term, uint8_t c)
{
    xassert(term->sixel.pan == 1);
    xassert(term->sixel.pad == 1);

    int col = term->sixel.pos.col;
    int width = term->sixel.image.width;

    if (unlikely(col >= width)) {
        resize_horizontally(term, col + count);
        width = term->sixel.image.width;
        count = min(count, max(width - col, 0));

        if (unlikely(count == 0))
            return;
    }

    uint32_t *data = term->sixel.image.p;

    term->sixel.pos.col += 1;
    term->sixel.image.p += 1;
    term->sixel.image.bottom_pixel |= c;

    sixel_add_ar_11(term, data, width, term->sixel.color, c);
}

static void
sixel_add_many_ar_11(struct terminal *term, uint8_t c, unsigned count)
{
    xassert(term->sixel.pan == 1);
    xassert(term->sixel.pad == 1);

    int col = term->sixel.pos.col;
    int width = term->sixel.image.width;

    if (unlikely(col + count - 1 >= width)) {
        resize_horizontally(term, col + count);
        width = term->sixel.image.width;
        count = min(count, max(width - col, 0));

        if (unlikely(count == 0))
            return;
    }

    uint32_t color = term->sixel.color;
    uint32_t *data = term->sixel.image.p;
    uint32_t *end = data + count;

    term->sixel.pos.col += count;
    term->sixel.image.p = end;
    term->sixel.image.bottom_pixel |= c;

    for (; data < end; data++)
        sixel_add_ar_11(term, data, width, color, c);

}

IGNORE_WARNING("-Wpedantic")

static void
decsixel_generic(struct terminal *term, uint8_t c)
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
        term->sixel.repeat_count = 1;
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
             * We set, and keep, 'col' outside the image boundary when
             * we've reached the maximum image height, to avoid also
             * having to check the row vs image height in the common
             * path in sixel_add().
             */
            term->sixel.pos.col = 0;
            term->sixel.image.p = &term->sixel.image.data[term->sixel.pos.row * term->sixel.image.width];
        }
        break;

    case '-':  /* GNL - Graphical New Line */
        term->sixel.pos.row += 6 * term->sixel.pan;
        term->sixel.pos.col = 0;
        term->sixel.image.bottom_pixel = 0;
        term->sixel.image.p = &term->sixel.image.data[term->sixel.pos.row * term->sixel.image.width];

        if (term->sixel.pos.row >= term->sixel.image.alloc_height) {
            if (!resize_vertically(term, term->sixel.pos.row + 6 * term->sixel.pan))
                term->sixel.pos.col = term->sixel.max_width + 1 * term->sixel.pad;
        }
        break;

    case '?' ... '~':
        sixel_add_many_generic(term, c - 63, 1);
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

UNIGNORE_WARNINGS

static void
decsixel_ar_11(struct terminal *term, uint8_t c)
{
    if (likely(c >= '?' && c <= '~'))
        sixel_add_one_ar_11(term, c - 63);
    else
        decsixel_generic(term, c);
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

        if (likely(term->sixel.image.width == 0 &&
                   term->sixel.image.height == 0))
        {
            term->sixel.pan = pan;
            term->sixel.pad = pad;
        } else {
            /*
             * Unsure what the VT340 does...
             *
             * We currently do *not* handle changing pan/pad in the
             * middle of a sixel, since that means resizing/stretching
             * the existing image.
             *
             * I'm *guessing* the VT340 simply changes the aspect
             * ratio of all subsequent sixels. But, given the design
             * of our implementation (the entire sixel is written to a
             * single pixman image), we can't easily do that.
             */
            LOG_WARN("sixel: unsupported: pan/pad changed after printing sixels");
            pan = term->sixel.pan;
            pad = term->sixel.pad;
        }

        pv *= pan;
        ph *= pad;

        LOG_DBG("pan=%u, pad=%u (aspect ratio = %d:%d), size=%ux%u",
                pan, pad, pan, pad, ph, pv);

        /*
         * RA really only acts as a rectangular erase - it fills the
         * specified area with the sixel background color[^1]. Nothing
         * else. It does *not* affect cursor positioning.
         *
         * This means that if the emitted sixel is *smaller* than the
         * RA, the text cursor will be placed "inside" the RA area.
         *
         * This means it would be more correct to view the RA area as
         * a *separate* sixel image, that is then overlaid with the
         * actual sixel.
         *
         * Still, RA _is_ a hint - the final image is _likely_ going
         * to be this large. And, treating RA as a separate image
         * prevents us from pre-allocating the final sixel image.
         *
         * So we don't. We use the RA as a hint, and pre-allocates the
         * backing image buffer.
         *
         * [^1]: i.e. it's a NOP if the sixel is transparent
         */
        if (ph >= term->sixel.image.height && pv >= term->sixel.image.width &&
            ph <= term->sixel.max_height && pv <= term->sixel.max_width)
        {
            /*
             * TODO: always resize to a multiple of 6*pan?
             *
             * We're effectively doing that already, except
             * sixel.image.height is set to ph, instead of the
             * allocated height (which is always a multiple of 6*pan).
             *
             * If the user wants to emit a sixel that isn't a multiple
             * of 6 pixels, the bottom sixel rows should all be empty,
             * and (assuming a transparent sixel), trimmed when the
             * final image is generated.
             */
            resize(term, ph, pv);
        }

        term->sixel.state = SIXEL_DECSIXEL;

        /* Update DCS put handler, since pan/pad may have changed */
        term->vt.dcs.put_handler = pan == 1 && pad == 1
            ? &sixel_put_ar_11
            : &sixel_put_generic;

        if (likely(pan == 1 && pad == 1))
            decsixel_ar_11(term, c);
        else
            decsixel_generic(term, c);

        break;
    }
    }
}

IGNORE_WARNING("-Wpedantic")

static void
decgri_generic(struct terminal *term, uint8_t c)
{
    switch (c) {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9': {
        unsigned param = term->sixel.param;
        param *= 10;
        param += c - '0';
        term->sixel.repeat_count = term->sixel.param = param;
        break;
    }

    case '?' ... '~': {
        unsigned count = term->sixel.repeat_count;
        if (unlikely(count == 0)) {
            count = 1;
        }

        sixel_add_many_generic(term, c - 63, count);
        term->sixel.state = SIXEL_DECSIXEL;
        break;
    }

    default:
        term->sixel.state = SIXEL_DECSIXEL;
        term->vt.dcs.put_handler(term, c);
        break;
    }
}

UNIGNORE_WARNINGS

static void
decgri_ar_11(struct terminal *term, uint8_t c)
{
    if (likely(c >= '?' && c <= '~')) {
        unsigned count = term->sixel.repeat_count;
        if (unlikely(count == 0)) {
            count = 1;
        }

        sixel_add_many_ar_11(term, c - 63, count);
        term->sixel.state = SIXEL_DECSIXEL;
    } else
        decgri_generic(term, c);
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
                 * Sixel's HLS use the following primary color hues:
                 *  blue:  0°
                 *  red:   120°
                 *  green: 240°
                 *
                 * While "standard" HSL uses:
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

        if (likely(term->sixel.pan == 1 && term->sixel.pad == 1))
            decsixel_ar_11(term, c);
        else
            decsixel_generic(term, c);
        break;
    }
    }
}

static void
sixel_put_generic(struct terminal *term, uint8_t c)
{
    switch (term->sixel.state) {
    case SIXEL_DECSIXEL: decsixel_generic(term, c); break;
    case SIXEL_DECGRA: decgra(term, c); break;
    case SIXEL_DECGRI: decgri_generic(term, c); break;
    case SIXEL_DECGCI: decgci(term, c); break;
    }

    count++;
}

static void
sixel_put_ar_11(struct terminal *term, uint8_t c)
{
    switch (term->sixel.state) {
    case SIXEL_DECSIXEL: decsixel_ar_11(term, c); break;
    case SIXEL_DECGRA: decgra(term, c); break;
    case SIXEL_DECGRI: decgri_ar_11(term, c); break;
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
