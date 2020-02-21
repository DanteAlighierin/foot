#include "sixel.h"

#include <string.h>

#define LOG_MODULE "sixel"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "render.h"

#define max(x, y) ((x) > (y) ? (x) : (y))

static const size_t COLOR_COUNT = 1024;
static const size_t IMAGE_WIDTH = 800;
static const size_t IMAGE_HEIGHT = 800;

static size_t count;

void
sixel_init(struct terminal *term)
{
    assert(term->sixel.palette == NULL);
    assert(term->sixel.image == NULL);

    term->sixel.state = SIXEL_SIXEL;
    term->sixel.row = 0;
    term->sixel.col = 0;
    term->sixel.color_idx = 0;
    term->sixel.max_col = 0;
    term->sixel.param = 0;
    term->sixel.param_idx = 0;
    memset(term->sixel.params, 0, sizeof(term->sixel.params));
    term->sixel.palette = calloc(COLOR_COUNT, sizeof(term->sixel.palette[0]));
    term->sixel.image = calloc(IMAGE_WIDTH * IMAGE_HEIGHT, sizeof(term->sixel.image[0]));

    count = 0;

    /* TODO: default palette */
}

void
sixel_unhook(struct terminal *term)
{
    free(term->sixel.palette);
    term->sixel.palette = NULL;

    LOG_DBG("generating %dx%d pixman image", term->sixel.row * 6, term->sixel.max_col);

    if (term->sixel.col >= 0) {
        if (term->sixel.col > term->sixel.max_col)
            term->sixel.max_col = term->sixel.col;
        term->sixel.row++;
        term->sixel.col = 0;
    }

    struct sixel image = {
        .data = term->sixel.image,
        .width = term->sixel.max_col,
        .height = term->sixel.row * 6,
        .pos = (struct coord){term->cursor.point.col, term->grid->offset + term->cursor.point.row},
    };

    image.pix = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8,
        image.width, image.height,
        term->sixel.image,
        IMAGE_WIDTH * sizeof(uint32_t));

    tll_push_back(term->sixel_images, image);

    term->sixel.image = NULL;
    term->sixel.max_col = 0;
    term->sixel.col = 0;
    term->sixel.row = 0;

    const size_t lines = (image.height + term->cell_height - 1) / term->cell_height;
    for (size_t i = 0; i < lines; i++)
        term_linefeed(term);
    term_formfeed(term);
    render_refresh(term);
}

static void
sixel_add(struct terminal *term, uint32_t color, uint8_t sixel)
{
    LOG_DBG("adding sixel %02hhx using color 0x%06x", sixel, color);
    if (term->sixel.col >= IMAGE_WIDTH) {
        LOG_WARN("column outside image width");
        return;
    }
    if (term->sixel.row >= IMAGE_HEIGHT) {
        LOG_WARN("row outside image height");
        return;
    }

    for (int i = 0; i < 6; i++) {
        int bit = sixel & 1;
        sixel >>= 1;
        if (bit) {
            size_t idx = (term->sixel.row * 6 + i) * IMAGE_WIDTH + term->sixel.col;
            term->sixel.image[idx] = term->colors.alpha / 256 << 24 | color;
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
        LOG_DBG("repeating '%c' %u times", c, term->sixel.param);
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
