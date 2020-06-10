#include "sixel.h"

#include <string.h>
#include <limits.h>

#define LOG_MODULE "sixel"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "render.h"
#include "sixel-hls.h"
#include "util.h"

static size_t count;

void
sixel_fini(struct terminal *term)
{
    free(term->sixel.palette);
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
    term->sixel.image.data = malloc(1 * 6 * sizeof(term->sixel.image.data[0]));
    term->sixel.image.width = 1;
    term->sixel.image.height = 6;

    if (term->sixel.palette == NULL) {
        term->sixel.palette = calloc(
            term->sixel.palette_size, sizeof(term->sixel.palette[0]));
    }

    for (size_t i = 0; i < 1 * 6; i++)
        term->sixel.image.data[i] = term->colors.alpha / 256u << 24 | term->colors.bg;

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

static void
sixel_delete_at_point(struct terminal *term, int _row, int col)
{
    if (likely(tll_length(term->grid->sixel_images) == 0))
        return;

    tll_foreach(term->grid->sixel_images, it) {
        struct sixel *six = &it->item;

        const int row = (term->grid->offset + _row) & (term->grid->num_rows - 1);
        const int six_start = six->pos.row;
        const int six_end = six_start + six->rows - 1;

        if (row >= six_start && row <= six_end) {
            const int col_start = six->pos.col;
            const int col_end = six->pos.col + six->cols;

            if (col >= col_start && col < col_end) {
                sixel_erase(term, six);
                tll_remove(term->grid->sixel_images, it);
            }
        }
    }
}

void
sixel_delete_at_row(struct terminal *term, int _row)
{
    sixel_delete_at_point(term, _row, INT_MAX);
}

void
sixel_delete_in_range(struct terminal *term, int _start, int _end)
{
    assert(_end >= _start);

    if (likely(tll_length(term->grid->sixel_images) == 0))
        return;

    if (_start == _end)
        return sixel_delete_at_row(term, _start);

    tll_foreach(term->grid->sixel_images, it) {
        struct sixel *six = &it->item;

        const int start = (term->grid->offset + _start) & (term->grid->num_rows - 1);
        const int end = start + (_end - _start);
        const int six_start = six->pos.row;
        const int six_end = six_start + six->rows - 1;

        if ((start <= six_start && end >= six_start) ||  /* Crosses sixel start boundary */
            (start <= six_end && end >= six_end) ||      /* Crosses sixel end boundary */
            (start >= six_start && end <= six_end))      /* Fully within sixel range */
        {
            sixel_erase(term, six);
            tll_remove(term->grid->sixel_images, it);
        }
    }
}

void
sixel_delete_at_cursor(struct terminal *term)
{
    sixel_delete_at_point(
        term, term->grid->cursor.point.row, term->grid->cursor.point.col);
}

void
sixel_unhook(struct terminal *term)
{
    sixel_delete_at_cursor(term);

    struct sixel image = {
        .data = term->sixel.image.data,
        .width = term->sixel.image.width,
        .height = term->sixel.image.height,
        .rows = (term->sixel.image.height + term->cell_height - 1) / term->cell_height,
        .cols = (term->sixel.image.width + term->cell_width - 1) / term->cell_width,
        .pos = (struct coord){
            term->grid->cursor.point.col,
            (term->grid->offset + term->grid->cursor.point.row) & (term->grid->num_rows - 1)},
    };

    LOG_DBG("generating %dx%d pixman image", image.width, image.height);

    image.pix = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8,
        image.width, image.height,
        term->sixel.image.data,
        term->sixel.image.width * sizeof(uint32_t));

    term->sixel.image.data = NULL;
    term->sixel.image.width = 0;
    term->sixel.image.height = 0;
    term->sixel.max_col = 0;
    term->sixel.pos = (struct coord){0, 0};

    for (size_t i = 0; i < image.rows; i++)
        term_linefeed(term);
    term_formfeed(term);
    render_refresh(term);

    tll_push_back(term->grid->sixel_images, image);
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
    LOG_DBG("resizing image: %dx%d -> %dx%d",
            term->sixel.image.width, term->sixel.image.height,
            new_width, new_height);

    uint32_t *old_data = term->sixel.image.data;
    const int old_width = term->sixel.image.width;
    const int old_height = term->sixel.image.height;

    assert(new_width >= old_width);
    assert(new_height >= old_height);

    uint32_t *new_data = NULL;

    if (new_width == old_width) {
        /* Width (and thus stride) is the same, so we can simply
         * re-alloc the existing buffer */

        new_data = realloc(old_data, new_width * new_height * sizeof(uint32_t));
        if (new_data == NULL) {
            LOG_ERRNO("failed to reallocate sixel image buffer");
            return false;
        }

        assert(new_height > old_height);

    } else {
        /* Width (and thus stride) change - need to allocate a new buffer */
        assert(new_width > old_width);
        new_data = malloc(new_width * new_height * sizeof(uint32_t));

        /* Copy old rows, and initialize new columns to background color */
        for (int r = 0; r < old_height; r++) {
            memcpy(&new_data[r * new_width], &old_data[r * old_width], old_width * sizeof(uint32_t));

            for (int c = old_width; c < new_width; c++)
                new_data[r * new_width + c] = term->colors.alpha / 256u << 24 | term->colors.bg;
        }
        free(old_data);
    }

    /* Initialize new rows to background color */
    for (int r = old_height; r < new_height; r++) {
        for (int c = 0; c < new_width; c++)
            new_data[r * new_width + c] = term->colors.alpha / 256u << 24 | term->colors.bg;
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
        term->sixel.pos.row * 6 + 5 >= term->sixel.image.height)
    {
        int width = max(
            term->sixel.image.width,
            max(term->sixel.max_col, term->sixel.pos.col + 1));

        int height = max(
            term->sixel.image.height,
            (term->sixel.pos.row + 1) * 6);

        resize(term, width, height);
    }

    for (int i = 0; i < 6; i++, sixel >>= 1) {
        if (sixel & 1) {
            size_t pixel_row = term->sixel.pos.row * 6 + i;
            size_t stride = term->sixel.image.width;
            size_t idx = pixel_row * stride + term->sixel.pos.col;
            term->sixel.image.data[idx] = term->colors.alpha / 256u << 24 | color;
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

    case '?'...'~':
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
    case '0'...'9':
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
            resize(term, ph, pv);
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
    case '0'...'9':
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
    case '0'...'9':
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
