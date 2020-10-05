#include "sixel.h"

#include <string.h>
#include <limits.h>

#define LOG_MODULE "sixel"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "render.h"
#include "sixel-hls.h"
#include "util.h"
#include "xmalloc.h"

static size_t count;

void
sixel_fini(struct terminal *term)
{
    free(term->sixel.palette);
}

static uint32_t
color_with_alpha(const struct terminal *term, uint32_t color)
{
    uint16_t alpha = color == term->colors.bg ? term->colors.alpha : 0xffff;
    return (alpha / 256u) << 24 | color;
}

void
sixel_init(struct terminal *term)
{
    assert(term->sixel.image.data == NULL);
    assert(term->sixel.palette_size <= SIXEL_MAX_COLORS);

    term->sixel.state = SIXEL_DECSIXEL;
    term->sixel.pos = (struct coord){0, 0};
    term->sixel.color_idx = 0;
    term->sixel.max_col = 0;
    term->sixel.param = 0;
    term->sixel.param_idx = 0;
    memset(term->sixel.params, 0, sizeof(term->sixel.params));
    term->sixel.image.data = xmalloc(1 * 6 * sizeof(term->sixel.image.data[0]));
    term->sixel.image.width = 1;
    term->sixel.image.height = 6;
    term->sixel.image.autosize = true;

    if (term->sixel.palette == NULL) {
        term->sixel.palette = xcalloc(
            term->sixel.palette_size, sizeof(term->sixel.palette[0]));
    }

    for (size_t i = 0; i < 1 * 6; i++)
        term->sixel.image.data[i] = color_with_alpha(term, term->colors.bg);

    count = 0;

    /* TODO: default palette */
}

void
sixel_destroy(struct sixel *sixel)
{
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

        for (int c = 0; c < term->grid->num_cols; c++)
            row->cells[c].attrs.clean = 0;
    }

    sixel_destroy(sixel);
}

/*
 * Calculates the scrollback relative row number, given an absolute row number.
 *
 * The scrollback relative row number 0 is the *first*, and *oldest*
 * row in the scrollback history (and thus the *first* row to be
 * scrolled out). Thus, a higher number means further *down* in the
 * scrollback, with the *highest* number being at the bottom of the
 * screen, where new input appears.
 */
static int
rebase_row(const struct terminal *term, int abs_row)
{
    int scrollback_start = term->grid->offset + term->rows;
    int rebased_row = abs_row - scrollback_start + term->grid->num_rows;

    rebased_row &= term->grid->num_rows - 1;
    return rebased_row;
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
        int row = rebase_row(term, it->item.pos.row + it->item.rows - 1);
        int col = it->item.pos.col;
        int col_count = it->item.cols;

        assert(row <= prev_row);

        if (row == prev_row) {
            /* Allowed to be on the same row only if their columns
             * don't overlap */

            assert(col + col_count <= prev_col ||
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

        assert(six->pos.row >= 0);
        assert(six->pos.row < term->grid->num_rows);

        int end = (six->pos.row + six->rows - 1) & (term->grid->num_rows - 1);
        assert(end >= six->pos.row);
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

            assert(!pixman_region32_not_empty(&intersection));

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
    verify_no_overlap(term);
    verify_list_order(term);
}

static void
sixel_insert(struct terminal *term, struct sixel sixel)
{
    int end_row = rebase_row(term, sixel.pos.row + sixel.rows - 1);

    tll_foreach(term->grid->sixel_images, it) {
        if (rebase_row(term, it->item.pos.row + it->item.rows - 1) < end_row) {
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

        int six_start = rebase_row(term, six->pos.row);

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

    verify_sixels(term);
}

void
sixel_scroll_down(struct terminal *term, int rows)
{
    if (likely(tll_length(term->grid->sixel_images) == 0))
        return;

    assert(term->grid->num_rows >= rows);

    tll_foreach(term->grid->sixel_images, it) {
        struct sixel *six = &it->item;

        int six_end = rebase_row(term, six->pos.row + six->rows - 1);
        if (six_end >= term->grid->num_rows - rows) {
            sixel_erase(term, six);
            tll_remove(term->grid->sixel_images, it);
        } else
            break;
    }

    verify_sixels(term);
}

static void
sixel_overwrite(struct terminal *term, struct sixel *six,
                int row, int col, int height, int width)
{
    int rel_above = min(max(row - six->pos.row, 0), six->rows);
    int rel_below = max(min(row + height - six->pos.row, six->rows), 0);
    int rel_left = min(max(col - six->pos.col, 0), six->cols);
    int rel_right = max(min(col + width - six->pos.col, six->cols), 0);

    assert(rel_above >= 0);
    assert(rel_below >= 0);
    assert(rel_left >= 0);
    assert(rel_right >= 0);

    LOG_DBG("SPLIT: six (%p): %dx%d-%dx%d, %dx%d-%dx%d, rel: above=%d, below=%d, left=%d, right=%d",
            (void *)six, six->pos.row, six->pos.col, six->rows, six->cols,
            row, col, height, width,
            rel_above, rel_below, rel_left, rel_right);

    struct sixel imgs[4] = {0};

    if (rel_above > 0) {
        imgs[0] =  (struct sixel){
            .width = six->width,
            .height = rel_above * term->cell_height,
            .pos = six->pos,
        };
        imgs[0].data = xmalloc(imgs[0].width * imgs[0].height * sizeof(uint32_t));
        memcpy(imgs[0].data, six->data, imgs[0].width * imgs[0].height * sizeof(uint32_t));
    }

    if (rel_below < six->rows) {
        imgs[1] = (struct sixel){
            .width = six->width,
            .height = six->height - rel_below * term->cell_height,
            .pos = (struct coord){
                six->pos.col,
                (six->pos.row + rel_below) & (term->grid->num_rows - 1)},
        };
        imgs[1].data = xmalloc(imgs[1].width * imgs[1].height * sizeof(uint32_t));
        memcpy(
            imgs[1].data,
            &((const uint32_t *)six->data)[rel_below * term->cell_height * six->width],
            imgs[1].width * imgs[1].height * sizeof(uint32_t));
    }

    if (rel_left > 0) {
        imgs[2] = (struct sixel){
            .width = rel_left * term->cell_width,
            .height = min(term->cell_height, six->height - rel_above * term->cell_height),
            .pos = (struct coord){
                six->pos.col,
                (six->pos.row + rel_above) & (term->grid->num_rows - 1)},
        };
        imgs[2].data = xmalloc(imgs[2].width * imgs[2].height * sizeof(uint32_t));
        for (size_t i = 0; i < imgs[2].height; i++)
            memcpy(
                &((uint32_t *)imgs[2].data)[i * imgs[2].width],
                &((const uint32_t *)six->data)[(rel_above * term->cell_height + i) * six->width],
                imgs[2].width * sizeof(uint32_t));
    }

    if (rel_right < six->cols) {
        imgs[3] = (struct sixel){
            .width = six->width - rel_right * term->cell_width,
            .height = min(term->cell_height, six->height - rel_above * term->cell_height),
            .pos = (struct coord){
                six->pos.col + rel_right,
                (six->pos.row + rel_above) & (term->grid->num_rows - 1)},
        };
        imgs[3].data = xmalloc(imgs[3].width * imgs[3].height * sizeof(uint32_t));
        for (size_t i = 0; i < imgs[3].height; i++)
            memcpy(
                &((uint32_t *)imgs[3].data)[i * imgs[3].width],
                &((const uint32_t *)six->data)[(rel_above * term->cell_height + i) * six->width + rel_right * term->cell_width],
                imgs[3].width * sizeof(uint32_t));
    }

    for (size_t i = 0; i < sizeof(imgs) / sizeof(imgs[0]); i++) {
        if (imgs[i].data == NULL)
            continue;

        imgs[i].rows = (imgs[i].height + term->cell_height - 1) / term->cell_height;
        imgs[i].cols = (imgs[i].width + term->cell_width - 1) / term->cell_width;

        imgs[i].pix = pixman_image_create_bits_no_clear(
            PIXMAN_a8r8g8b8,
            imgs[i].width, imgs[i].height,
            imgs[i].data, imgs[i].width * sizeof(uint32_t));
        sixel_insert(term, imgs[i]);
    }
}

/* Row numbers are absolute */
static void
_sixel_overwrite_by_rectangle(
    struct terminal *term, int row, int col, int height, int width)
{
    const int start = row;
    const int end = row + height - 1;

    /* We should never generate scrollback wrapping sixels */
    assert(end < term->grid->num_rows);

    const int scrollback_rel_start = rebase_row(term, start);

    tll_foreach(term->grid->sixel_images, it) {
        struct sixel *six = &it->item;

        const int six_start = six->pos.row;
        const int six_end = (six_start + six->rows - 1);
        const int six_scrollback_rel_end = rebase_row(term, six_end);

        /* We should never generate scrollback wrapping sixels */
        assert(six_end < term->grid->num_rows);

        if (six_scrollback_rel_end < scrollback_rel_start) {
            /* All remaining sixels are *before* our rectangle */
            break;
        }

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
                struct sixel to_be_erased = *six;
                tll_remove(term->grid->sixel_images, it);

                sixel_overwrite(term, &to_be_erased, start, col, height, width);
                sixel_erase(term, &to_be_erased);
            }
        }
    }
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
        assert(height - rows_to_wrap_around > 0);
        _sixel_overwrite_by_rectangle(term, start, col, rows_to_wrap_around, width);
        _sixel_overwrite_by_rectangle(term, 0, col, height - rows_to_wrap_around, width);
    } else
        _sixel_overwrite_by_rectangle(term, start, col, height, width);
}

/* Row numbers are relative to grid offset */
void
sixel_overwrite_by_row(struct terminal *term, int _row, int col, int width)
{
    assert(col >= 0);

    assert(_row >= 0);
    assert(_row < term->rows);
    assert(col >= 0);
    assert(col < term->grid->num_cols);

    if (likely(tll_length(term->grid->sixel_images) == 0))
        return;

    if (col + width > term->grid->num_cols)
        width = term->grid->num_cols - col;

    const int row = (term->grid->offset + _row) & (term->grid->num_rows - 1);
    const int scrollback_rel_row = rebase_row(term, row);

    tll_foreach(term->grid->sixel_images, it) {
        struct sixel *six = &it->item;
        const int six_start = six->pos.row;
        const int six_end = (six_start + six->rows - 1) & (term->grid->num_rows - 1);

        /* We should never generate scrollback wrapping sixels */
        assert(six_end >= six_start);

        const int six_scrollback_rel_end = rebase_row(term, six_end);

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

                sixel_overwrite(term, &to_be_erased, row, col, 1, width);
                sixel_erase(term, &to_be_erased);
            }
        }
    }
}

void
sixel_overwrite_at_cursor(struct terminal *term, int width)
{
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

            /* Sixels that didn’t overlap may now do so, which isn’t
             * allowed of course */
            _sixel_overwrite_by_rectangle(
                term, six->pos.row, six->pos.col, six->rows, six->cols);
            sixel_insert(term, it->item);
        }

        tll_free(copy);
    }

    term->grid = g;
}

void
sixel_unhook(struct terminal *term)
{
    int pixel_row_idx = 0;
    int pixel_rows_left = term->sixel.image.height;
    const int stride = term->sixel.image.width * sizeof(uint32_t);

    /*
     * Need to 'remember' current cursor column.
     *
     * If we split up the sixel (to avoid scrollback wrap-around), we
     * will emit a carriage-return (after several linefeeds), which
     * will reset the cursor column to 0. If we use _that_ column for
     * the subsequent image parts, the image will look sheared.
     */
    const int start_col = term->grid->cursor.point.col;

    /* We do not allow sixels to cross the scrollback wrap-around, as
     * this makes intersection calculations much more complicated */
    while (pixel_rows_left > 0) {
        const struct coord *cursor = &term->grid->cursor.point;

        const int cur_row = (term->grid->offset + cursor->row) & (term->grid->num_rows - 1);
        const int rows_avail = term->grid->num_rows - cur_row;

        const int pixel_rows_avail = rows_avail * term->cell_height;

        const int width = term->sixel.image.width;
        const int height = min(pixel_rows_left, pixel_rows_avail);

        uint32_t *img_data;
        if (pixel_row_idx == 0)
            img_data = term->sixel.image.data;
        else {
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
        };

        sixel_overwrite_by_rectangle(
            term, cursor->row, image.pos.col, image.rows, image.cols);

        LOG_DBG("generating %dx%d pixman image at %d-%d",
                image.width, image.height,
                image.pos.row, image.pos.row + image.rows);

        image.pix = pixman_image_create_bits_no_clear(
            PIXMAN_a8r8g8b8,
            image.width, image.height,
            img_data, stride);

        /* Allocate space *first*, then insert */
        for (size_t i = 0; i < image.rows; i++)
            term_linefeed(term);
        term_carriage_return(term);

        sixel_insert(term, image);

        pixel_row_idx += height;
        pixel_rows_left -= height;
    }

    term->sixel.image.data = NULL;
    term->sixel.image.width = 0;
    term->sixel.image.height = 0;
    term->sixel.max_col = 0;
    term->sixel.pos = (struct coord){0, 0};

    LOG_DBG("you now have %zu sixels in current grid",
            tll_length(term->grid->sixel_images));

    render_refresh(term);
}

static unsigned
max_width(const struct terminal *term)
{
    /* foot extension - treat 0 to mean current terminal size */
    return term->sixel.max_width == 0
        ? term->cols * term->cell_width
        : term->sixel.max_width;
}

static unsigned
max_height(const struct terminal *term)
{
    /* foot extension - treat 0 to mean current terminal size */
    return term->sixel.max_height == 0
        ? term->rows * term->cell_height
        : term->sixel.max_height;
}

static bool
resize(struct terminal *term, int new_width, int new_height)
{
    if (!term->sixel.image.autosize)
        return false;

    LOG_DBG("resizing image: %dx%d -> %dx%d",
            term->sixel.image.width, term->sixel.image.height,
            new_width, new_height);

    uint32_t *old_data = term->sixel.image.data;
    const int old_width = term->sixel.image.width;
    const int old_height = term->sixel.image.height;

    int alloc_new_width = new_width;
    int alloc_new_height = (new_height + 6 - 1) / 6 * 6;
    assert(alloc_new_height >= new_height);
    assert(alloc_new_height - new_height < 6);

    assert(new_width >= old_width);
    assert(new_height >= old_height);

    uint32_t *new_data = NULL;

    if (new_width == old_width) {
        /* Width (and thus stride) is the same, so we can simply
         * re-alloc the existing buffer */

        new_data = realloc(old_data, alloc_new_width * alloc_new_height * sizeof(uint32_t));
        if (new_data == NULL) {
            LOG_ERRNO("failed to reallocate sixel image buffer");
            return false;
        }

        assert(new_height > old_height);

    } else {
        /* Width (and thus stride) change - need to allocate a new buffer */
        assert(new_width > old_width);
        new_data = xmalloc(alloc_new_width * alloc_new_height * sizeof(uint32_t));

        /* Copy old rows, and initialize new columns to background color */
        for (int r = 0; r < old_height; r++) {
            memcpy(&new_data[r * new_width], &old_data[r * old_width], old_width * sizeof(uint32_t));

            for (int c = old_width; c < new_width; c++)
                new_data[r * new_width + c] = color_with_alpha(term, term->colors.bg);
        }
        free(old_data);
    }

    /* Initialize new rows to background color */
    for (int r = old_height; r < new_height; r++) {
        for (int c = 0; c < new_width; c++)
            new_data[r * new_width + c] = color_with_alpha(term, term->colors.bg);
    }

    assert(new_data != NULL);
    term->sixel.image.data = new_data;
    term->sixel.image.width = new_width;
    term->sixel.image.height = new_height;

    return true;
}

static void
sixel_add(struct terminal *term, uint32_t color, uint8_t sixel)
{
    //LOG_DBG("adding sixel %02hhx using color 0x%06x", sixel, color);

    if (term->sixel.pos.col >= max_width(term) ||
        term->sixel.pos.row * 6 + 5 >= max_height(term))
    {
        return;
    }

    if (term->sixel.pos.col >= term->sixel.image.width ||
        term->sixel.pos.row * 6 + 5 >= (term->sixel.image.height + 6 - 1) / 6 * 6)
    {
        int width = max(
            term->sixel.image.width,
            max(term->sixel.max_col, term->sixel.pos.col + 1));

        int height = max(
            term->sixel.image.height,
            (term->sixel.pos.row + 1) * 6);

        if (!resize(term, width, height))
            return;
    }

    for (int i = 0; i < 6; i++, sixel >>= 1) {
        if (sixel & 1) {
            size_t pixel_row = term->sixel.pos.row * 6 + i;
            size_t stride = term->sixel.image.width;
            size_t idx = pixel_row * stride + term->sixel.pos.col;
            term->sixel.image.data[idx] = color_with_alpha(term, color);
        }
    }

    assert(sixel == 0);
    term->sixel.pos.col++;
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
        if (term->sixel.pos.col > term->sixel.max_col)
            term->sixel.max_col = term->sixel.pos.col;
        term->sixel.pos.col = 0;
        break;

    case '-':
        if (term->sixel.pos.col > term->sixel.max_col)
            term->sixel.max_col = term->sixel.pos.col;
        term->sixel.pos.row++;
        term->sixel.pos.col = 0;
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
        sixel_add(term, term->sixel.palette[term->sixel.color_idx], c - 63);
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
            ph <= max_height(term) && pv <= max_width(term))
        {
            if (resize(term, ph, pv))
                term->sixel.image.autosize = false;
        }

        term->sixel.state = SIXEL_DECSIXEL;
        sixel_put(term, c);
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

    default:
        //LOG_DBG("repeating '%c' %u times", c, term->sixel.param);
        for (unsigned i = 0; i < term->sixel.param; i++)
            decsixel(term, c);
        term->sixel.state = SIXEL_DECSIXEL;
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
            unsigned c1 = term->sixel.params[2];
            unsigned c2 = term->sixel.params[3];
            unsigned c3 = term->sixel.params[4];

            switch (format) {
            case 1: { /* HLS */
                uint32_t rgb = hls_to_rgb(c1, c2, c3);
                LOG_DBG("setting palette #%d = HLS %hhu/%hhu/%hhu (0x%06x)",
                        term->sixel.color_idx, c1, c2, c3, rgb);
                term->sixel.palette[term->sixel.color_idx] = rgb;
                break;
            }

            case 2: {  /* RGB */
                uint8_t r = 255 * c1 / 100;
                uint8_t g = 255 * c2 / 100;
                uint8_t b = 255 * c3 / 100;

                LOG_DBG("setting palette #%d = RGB %hhu/%hhu/%hhu",
                        term->sixel.color_idx, r, g, b);

                term->sixel.palette[term->sixel.color_idx] = r << 16 | g << 8 | b;
                break;
            }
            }
        }

        term->sixel.state = SIXEL_DECSIXEL;
        sixel_put(term, c);
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
    snprintf(reply, sizeof(reply), "\033[?1;0;%uS", term->sixel.palette_size);
    term_to_slave(term, reply, strlen(reply));
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

    free(term->sixel.palette);
    term->sixel.palette = NULL;

    term->sixel.palette_size = new_palette_size;
    sixel_colors_report_current(term);
}

void
sixel_colors_report_max(struct terminal *term)
{
    char reply[24];
    snprintf(reply, sizeof(reply), "\033[?1;0;%uS", SIXEL_MAX_COLORS);
    term_to_slave(term, reply, strlen(reply));
    LOG_DBG("query response for max color count: %u", SIXEL_MAX_COLORS);
}

void
sixel_geometry_report_current(struct terminal *term)
{
    char reply[64];
    snprintf(reply, sizeof(reply), "\033[?2;0;%u;%uS",
             max_width(term), max_height(term));
    term_to_slave(term, reply, strlen(reply));

    LOG_DBG("query response for current sixel geometry: %ux%u",
            max_width(term), max_height(term));
}

void
sixel_geometry_reset(struct terminal *term)
{
    LOG_DBG("sixel geometry reset to %ux%u", max_width(term), max_height(term));
    term->sixel.max_width = 0;
    term->sixel.max_height = 0;
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
    unsigned max_width = term->cols * term->cell_width;
    unsigned max_height = term->rows * term->cell_height;

    char reply[64];
    snprintf(reply, sizeof(reply), "\033[?2;0;%u;%uS", max_width, max_height);
    term_to_slave(term, reply, strlen(reply));

    LOG_DBG("query response for max sixel geometry: %ux%u",
            max_width, max_height);
}
