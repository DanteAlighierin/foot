#include "sixel.h"

#include <string.h>

#define LOG_MODULE "sixel"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "render.h"

#define ALEN(v) (sizeof(v) / sizeof(v[0]))
#define max(x, y) ((x) > (y) ? (x) : (y))
#define min(x, y) ((x) < (y) ? (x) : (y))

static const size_t COLOR_COUNT = 1024;

static size_t count;

void
sixel_init(struct terminal *term)
{
    assert(term->sixel.palette == NULL);
    assert(term->sixel.image.data == NULL);

    term->sixel.state = SIXEL_DECSIXEL;
    term->sixel.pos = (struct coord){0, 0};
    term->sixel.color_idx = 0;
    term->sixel.max_col = 0;
    term->sixel.param = 0;
    term->sixel.param_idx = 0;
    memset(term->sixel.params, 0, sizeof(term->sixel.params));
    term->sixel.palette = calloc(COLOR_COUNT, sizeof(term->sixel.palette[0]));
    term->sixel.image.data = malloc(1 * 6 * sizeof(term->sixel.image.data[0]));
    term->sixel.image.width = 1;
    term->sixel.image.height = 6;

    for (size_t i = 0; i < 1 * 6; i++)
        term->sixel.image.data[i] = term->colors.alpha / 256 << 24 | term->colors.bg;

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

    struct sixel image = {
        .data = term->sixel.image.data,
        .width = term->sixel.image.width,
        .height = term->sixel.image.height,
        .rows = (term->sixel.image.height + term->cell_height - 1) / term->cell_height,
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
    term->sixel.pos = (struct coord){0, 0};

    for (size_t i = 0; i < image.rows; i++)
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

    } else {
        /* Width (and thus stride) change - need to allocate a new buffer */
        assert(new_width > old_width);
        new_data = malloc(new_width * new_height * sizeof(uint32_t));

        /* Copy old rows, and initialize new columns to background color */
        for (int r = 0; r < old_height; r++) {
            memcpy(&new_data[r * new_width], &old_data[r * old_width], old_width * sizeof(uint32_t));

            for (int c = old_width; c < new_width; c++)
                new_data[r * new_width + c] = term->colors.alpha / 256 << 24 | term->colors.bg;
        }
        free(old_data);
    }

    /* Initialize new rows to background color */
    for (int r = old_height; r < new_height; r++) {
        for (int c = 0; c < new_width; c++)
            new_data[r * new_width + c] = term->colors.alpha / 256 << 24 | term->colors.bg;
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

    if (term->sixel.pos.col >= term->sixel.image.width ||
        term->sixel.pos.row * 6 >= term->sixel.image.height)
    {
        resize(term,
               max(term->sixel.max_col, term->sixel.pos.col + 1),
               (term->sixel.pos.row + 1) * 6);
    }

    for (int i = 0; i < 6; i++, sixel >>= 1) {
        if (sixel & 1) {
            size_t pixel_row = term->sixel.pos.row * 6 + i;
            size_t stride = term->sixel.image.width;
            size_t idx = pixel_row * stride + term->sixel.pos.col;
            term->sixel.image.data[idx] = term->colors.alpha / 256 << 24 | color;
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
        LOG_WARN("invalid sixel charactwer: '%c' at idx=%zu", c, count);
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

        if (ph >= term->sixel.image.height && pv >= term->sixel.image.width)
            resize(term, ph, pv);

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

        if (nparams > 0) {
            /* Add one, as we use idx==0 for background color (TODO) */
            term->sixel.color_idx = min(1 + term->sixel.params[0], COLOR_COUNT - 1);
        }

        if (nparams > 4) {
            unsigned format = term->sixel.params[1];
            unsigned c1 = term->sixel.params[2];
            unsigned c2 = term->sixel.params[3];
            unsigned c3 = term->sixel.params[4];

            switch (format) {
            case 1: { /* HLS */
                LOG_ERR("HLS color format not implemented");
                assert(false && "HLS color format not implemented");
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
