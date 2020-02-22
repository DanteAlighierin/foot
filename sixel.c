#include "sixel.h"

#include <string.h>

#define LOG_MODULE "sixel"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "render.h"

#define max(x, y) ((x) > (y) ? (x) : (y))

static const size_t COLOR_COUNT = 1024;

static size_t count;

void
sixel_init(struct terminal *term)
{
    assert(term->sixel.palette == NULL);
    assert(term->sixel.image.data == NULL);

    term->sixel.state = SIXEL_SIXEL;
    term->sixel.row = 0;
    term->sixel.col = 0;
    term->sixel.color_idx = 0;
    term->sixel.max_col = 0;
    term->sixel.param = 0;
    term->sixel.param_idx = 0;
    memset(term->sixel.params, 0, sizeof(term->sixel.params));
    term->sixel.palette = calloc(COLOR_COUNT, sizeof(term->sixel.palette[0]));
    term->sixel.image.data = calloc(1 * 6, sizeof(term->sixel.image.data[0]));
    term->sixel.image.width = 1;
    term->sixel.image.height = 6;

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
sixel_unhook(struct terminal *term)
{
    free(term->sixel.palette);
    term->sixel.palette = NULL;

    if (term->sixel.col > term->sixel.max_col)
        term->sixel.max_col = term->sixel.col;
    term->sixel.row++;
    term->sixel.col = 0;

    struct sixel image = {
        .data = term->sixel.image.data,
        .width = term->sixel.max_col,
        .height = term->sixel.row * 6,
        .rows = (term->sixel.row * 6 + term->cell_height - 1) / term->cell_height,
        .pos = (struct coord){term->cursor.point.col, term->grid->offset + term->cursor.point.row},
    };

    LOG_DBG("generating %dx%d pixman image", image.width, image.height);

    image.pix = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8,
        image.width, image.height,
        term->sixel.image.data,
        term->sixel.image.width * sizeof(uint32_t));

    tll_foreach(term->sixel_images, it) {
        if (it->item.pos.row == image.pos.row) {
            sixel_destroy(&it->item);
            tll_remove(term->sixel_images, it);
        }
    }

    tll_push_back(term->sixel_images, image);

    term->sixel.image.data = NULL;
    term->sixel.image.width = 0;
    term->sixel.image.height = 0;
    term->sixel.max_col = 0;
    term->sixel.col = 0;
    term->sixel.row = 0;

    const size_t lines = (image.height + term->cell_height - 1) / term->cell_height;
    for (size_t i = 0; i < lines; i++)
        term_linefeed(term);
    term_formfeed(term);
    render_refresh(term);
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
        memset(&new_data[old_height * new_width], 0, (new_height - old_height) * new_width * sizeof(uint32_t));
    } else {
        /* Width (and thus stride) change - need to allocate a new buffer */
        assert(new_width > old_width);
        new_data = malloc(new_width * new_height * sizeof(uint32_t));

        /* Copy old rows, and zero-initialize the tail of each row */
        for (int r = 0; r < old_height; r++) {
            memcpy(&new_data[r * new_width], &old_data[r * old_width], old_width * sizeof(uint32_t));
            memset(&new_data[r * new_width + old_width], 0, (new_width - old_width) * sizeof(uint32_t));
        }

        /* Zero-initiailize new rows */
        for (int r = old_height; r < new_height; r++)
            memset(&new_data[r * new_width], 0, new_width * sizeof(uint32_t));

        free(old_data);
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

    if (term->sixel.col >= term->sixel.image.width ||
        term->sixel.row * 6 >= term->sixel.image.height)
    {
        resize(term,
               max(term->sixel.max_col, term->sixel.col + 1),
               (term->sixel.row + 1) * 6);
    }

    for (int i = 0; i < 6; i++, sixel >>= 1) {
        if (sixel & 1) {
            size_t pixel_row = term->sixel.row * 6 + i;
            size_t stride = term->sixel.image.width;
            size_t idx = pixel_row * stride + term->sixel.col;
            term->sixel.image.data[idx] = term->colors.alpha / 256 << 24 | color;
        }
    }

    assert(sixel == 0);

    term->sixel.col++;
}

static void
sixel_sixel(struct terminal *term, uint8_t c)
{
    switch (c) {
    case '"':
        term->sixel.state = SIXEL_RASTER;
        term->sixel.param = 0;
        term->sixel.param_idx = 0;
        break;

    case '#':
        term->sixel.state = SIXEL_COLOR;
        term->sixel.color_idx = 0;
        term->sixel.param = 0;
        term->sixel.param_idx = 0;
        break;

    case '$':
        if (term->sixel.col > term->sixel.max_col)
            term->sixel.max_col = term->sixel.col;
        term->sixel.col = 0;
        break;

    case '-':
        if (term->sixel.col > term->sixel.max_col)
            term->sixel.max_col = term->sixel.col;
        term->sixel.row++;
        term->sixel.col = 0;
        break;

    case '!':
        term->sixel.state = SIXEL_REPEAT;
        term->sixel.param = 0;
        term->sixel.param_idx = 0;
        break;

    default:
        if (c < '?' || c > '~') {
            LOG_ERR("invalid sixel charactwer: '%c' at idx=%zu", c, count);
            return;
        }

        sixel_add(term, term->sixel.palette[term->sixel.color_idx], c - 63);
        break;
    }
}

static void
sixel_repeat(struct terminal *term, uint8_t c)
{
    if (c >= '0' && c <= '9') {
        term->sixel.param *= 10;
        term->sixel.param += c - '0';
    } else {
        //LOG_DBG("repeating '%c' %u times", c, term->sixel.param);

        term->sixel.state = SIXEL_SIXEL;
        for (unsigned i = 0; i < term->sixel.param; i++)
            sixel_sixel(term, c);
    }
}

static void
sixel_raster(struct terminal *term, uint8_t c)
{
    if (c >= '0' && c <= '9') {
        term->sixel.param *= 10;
        term->sixel.param += c - '0';
    } else {
        if (term->sixel.param_idx < sizeof(term->sixel.params) / sizeof(term->sixel.params[0]))
            term->sixel.params[term->sixel.param_idx++] = term->sixel.param;

        term->sixel.param = 0;

        if (c != ';') {
            unsigned pan __attribute__((unused)) = term->sixel.params[0];
            unsigned pad __attribute__((unused)) = term->sixel.params[1];
            unsigned ph __attribute__((unused)) = term->sixel.params[2];
            unsigned pv __attribute__((unused)) = term->sixel.params[3];

            LOG_DBG("pan=%u, pad=%u (aspect ratio = %u), size=%ux%u",
                    pan, pad, pan / pad, ph, pv);

            if (ph >= term->sixel.image.height && pv >= term->sixel.image.width)
                resize(term, ph, pv);
        }

        switch (c) {
        case '#': term->sixel.state = SIXEL_COLOR; break;
        case ';': term->sixel.state = SIXEL_RASTER; break;
        default:  term->sixel.state = SIXEL_SIXEL; sixel_sixel(term, c); break;
        }
    }
}

static void
sixel_color(struct terminal *term, uint8_t c)
{
    if (c >= '0' && c <= '9') {
        term->sixel.param *= 10;
        term->sixel.param += c - '0';
    } else {
        if (term->sixel.param < COLOR_COUNT)
            term->sixel.color_idx = term->sixel.param;
        else
            term->sixel.color_idx = 0;

        term->sixel.param_idx = 0;
        term->sixel.param = 0;

        switch (c) {
        case '#': term->sixel.state = SIXEL_COLOR; break;
        case ';': term->sixel.state = SIXEL_COLOR_SPEC; break;
        default:  term->sixel.state = SIXEL_SIXEL; sixel_sixel(term, c); break;
        }
    }
}

static void
sixel_color_spec(struct terminal *term, uint8_t c)
{
    if (c >= '0' && c <= '9') {
        term->sixel.param *= 10;
        term->sixel.param += c - '0';
    } else {
        if (term->sixel.param_idx < sizeof(term->sixel.params) / sizeof(term->sixel.params[0]))
            term->sixel.params[term->sixel.param_idx++] = term->sixel.param;

        term->sixel.param = 0;

        if (c != ';') {
            unsigned format = term->sixel.params[0];
            unsigned c1 = term->sixel.params[1];
            unsigned c2 = term->sixel.params[2];
            unsigned c3 = term->sixel.params[3];

            if (format == 1) {
                assert(false && "HLS color format not implemented");
            } else {
                uint8_t r = 255 * c1 / 100;
                uint8_t g = 255 * c2 / 100;
                uint8_t b = 255 * c3 / 100;

                LOG_DBG("setting palette #%d = RGB %hhu/%hhu/%hhu",
                        term->sixel.color_idx, r, g, b);

                term->sixel.palette[term->sixel.color_idx] = r << 16 | g << 8 | b;
            }
        }

        switch (c) {
        case '#': term->sixel.state = SIXEL_COLOR; break;
        case ';': term->sixel.state = SIXEL_COLOR_SPEC; break;
        default:  term->sixel.state = SIXEL_SIXEL; sixel_sixel(term, c); break;
        }
    }
}

void
sixel_put(struct terminal *term, uint8_t c)
{
    count++;
    switch (c) {
    case ' ': return;
    case '\n': return;
    case '\r': return;
    }

    switch (term->sixel.state) {
    case SIXEL_SIXEL: sixel_sixel(term, c); break;
    case SIXEL_REPEAT: sixel_repeat(term, c); break;
    case SIXEL_RASTER: sixel_raster(term, c); break;
    case SIXEL_COLOR: sixel_color(term, c); break;
    case SIXEL_COLOR_SPEC: sixel_color_spec(term, c); break;
    }
}
