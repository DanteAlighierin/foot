#include "font.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include <assert.h>
#include <threads.h>

#define LOG_MODULE "font"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "stride.h"

#define min(x, y) ((x) < (y) ? (x) : (y))

static FT_Library ft_lib;
static mtx_t ft_lock;

static const size_t cache_size = 512;

static void __attribute__((constructor))
init(void)
{
    FcInit();
    FT_Init_FreeType(&ft_lib);
    mtx_init(&ft_lock, mtx_plain);
}

static void __attribute__((destructor))
fini(void)
{
    mtx_destroy(&ft_lock);
    FT_Done_FreeType(ft_lib);
    FcFini();
}

static bool
from_font_set(FcPattern *pattern, FcFontSet *fonts, int start_idx, const font_list_t *fallbacks,
              const char *attributes, struct font *font, bool is_fallback)
{
    memset(font, 0, sizeof(*font));

    size_t attr_len = attributes == NULL ? 0 : strlen(attributes);
    bool have_attrs = attr_len > 0;

    FcChar8 *face_file = NULL;
    FcPattern *final_pattern = NULL;
    int font_idx = -1;

    for (int i = start_idx; i < fonts->nfont; i++) {
        FcPattern *pat = FcFontRenderPrepare(NULL, pattern, fonts->fonts[i]);
        assert(pat != NULL);

        if (FcPatternGetString(pat, FC_FT_FACE, 0, &face_file) != FcResultMatch) {
            if (FcPatternGetString(pat, FC_FILE, 0, &face_file) != FcResultMatch) {
                FcPatternDestroy(pat);
                continue;
            }
        }

        final_pattern = pat;
        font_idx = i;
        break;
    }

    assert(font_idx != -1);
    assert(final_pattern != NULL);

    double dpi;
    if (FcPatternGetDouble(final_pattern, FC_DPI, 0, &dpi) != FcResultMatch)
        dpi = 96;

    double size;
    if (FcPatternGetDouble(final_pattern, FC_PIXEL_SIZE, 0, &size)) {
        LOG_ERR("%s: failed to get size", face_file);
        FcPatternDestroy(final_pattern);
        return false;
    }

    FcBool scalable;
    if (FcPatternGetBool(final_pattern, FC_SCALABLE, 0, &scalable) != FcResultMatch)
        scalable = FcTrue;

    double pixel_fixup;
    if (FcPatternGetDouble(final_pattern, "pixelsizefixupfactor", 0, &pixel_fixup) != FcResultMatch)
        pixel_fixup = 1.;

    LOG_DBG("loading: %s", face_file);

    mtx_lock(&ft_lock);
    FT_Face ft_face;
    FT_Error ft_err = FT_New_Face(ft_lib, (const char *)face_file, 0, &ft_face);
    mtx_unlock(&ft_lock);
    if (ft_err != 0)
        LOG_ERR("%s: failed to create FreeType face", face_file);

    if ((ft_err = FT_Set_Char_Size(ft_face, size * 64, 0, 0, 0)) != 0)
        LOG_WARN("failed to set character size");

    FcBool fc_hinting;
    if (FcPatternGetBool(final_pattern, FC_HINTING,0,  &fc_hinting) != FcResultMatch)
        fc_hinting = FcTrue;

    FcBool fc_antialias;
    if (FcPatternGetBool(final_pattern, FC_ANTIALIAS, 0, &fc_antialias) != FcResultMatch)
        fc_antialias = FcTrue;

    int fc_hintstyle;
    if (FcPatternGetInteger(final_pattern, FC_HINT_STYLE, 0, &fc_hintstyle) != FcResultMatch)
        fc_hintstyle = FC_HINT_SLIGHT;

    int fc_rgba;
    if (FcPatternGetInteger(final_pattern, FC_RGBA, 0, &fc_rgba) != FcResultMatch)
        fc_rgba = FC_RGBA_UNKNOWN;

    int load_flags = 0;
    if (!fc_antialias) {
        if (!fc_hinting || fc_hintstyle == FC_HINT_NONE)
            load_flags |= FT_LOAD_MONOCHROME | FT_LOAD_NO_HINTING | FT_LOAD_TARGET_NORMAL;
        else
            load_flags |= FT_LOAD_MONOCHROME | FT_LOAD_TARGET_MONO;
    } else {
        if (!fc_hinting || fc_hintstyle == FC_HINT_NONE)
            load_flags |= FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING | FT_LOAD_TARGET_NORMAL;
        else if (fc_hinting && fc_hintstyle == FC_HINT_SLIGHT)
            load_flags |= FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT;
        else if (fc_rgba == FC_RGBA_RGB)
            load_flags |= FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD;
        else if (fc_rgba == FC_RGBA_VRGB)
            load_flags |= FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD_V;
        else
            load_flags |= FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL;
    }

    FcBool fc_embeddedbitmap;
    if (FcPatternGetBool(final_pattern, FC_EMBEDDED_BITMAP, 0, &fc_embeddedbitmap) != FcResultMatch)
        fc_embeddedbitmap = FcTrue;

    if (!fc_embeddedbitmap)
        load_flags |= FT_LOAD_NO_BITMAP;

    int render_flags = 0;
    if (!fc_antialias)
        render_flags |= FT_RENDER_MODE_MONO;
    else {
        if (fc_rgba == FC_RGBA_RGB)
            render_flags |= FT_RENDER_MODE_LCD;
        else if (fc_rgba == FC_RGBA_VRGB)
            render_flags |= FT_RENDER_MODE_LCD_V;
        else
            render_flags |= FT_RENDER_MODE_NORMAL;
    }

    int fc_lcdfilter;
    if (FcPatternGetInteger(final_pattern, FC_LCD_FILTER, 0, &fc_lcdfilter) != FcResultMatch)
        fc_lcdfilter = FC_LCD_DEFAULT;

    switch (fc_lcdfilter) {
    case FC_LCD_NONE:    font->lcd_filter = FT_LCD_FILTER_NONE; break;
    case FC_LCD_DEFAULT: font->lcd_filter = FT_LCD_FILTER_DEFAULT; break;
    case FC_LCD_LIGHT:   font->lcd_filter = FT_LCD_FILTER_LIGHT; break;
    case FC_LCD_LEGACY:  font->lcd_filter = FT_LCD_FILTER_LEGACY; break;
    }

    FcPatternDestroy(final_pattern);

    mtx_init(&font->lock, mtx_plain);
    font->face = ft_face;
    font->load_flags = load_flags | FT_LOAD_COLOR;
    font->render_flags = render_flags;
    font->is_fallback = is_fallback;
    font->pixel_size_fixup = scalable ? pixel_fixup : 1.;
    font->fc_idx = font_idx;

    if (!is_fallback) {
        font->fc_pattern = pattern;
        font->fc_fonts = fonts;
        font->cache = calloc(cache_size, sizeof(font->cache[0]));
    }

    if (fallbacks != NULL) {
        tll_foreach(*fallbacks, it) {
            size_t len = strlen(it->item) + (have_attrs ? 1 : 0) + attr_len + 1;
            char *fallback = malloc(len);

            strcpy(fallback, it->item);
            if (have_attrs) {
                strcat(fallback, ":");
                strcat(fallback, attributes);
            }

            LOG_DBG("%s: adding fallback: %s", it->item, fallback);
            tll_push_back(font->fallbacks, fallback);
        }
    }

    return true;
}

static bool
from_name(const char *base_name, const font_list_t *fallbacks, const char *attributes, struct font *font, bool is_fallback)
{
    size_t attr_len = attributes == NULL ? 0 : strlen(attributes);
    bool have_attrs = attr_len > 0;

    char name[strlen(base_name) + (have_attrs ? 1 : 0) + attr_len + 1];
    strcpy(name, base_name);
    if (have_attrs){
        strcat(name, ":");
        strcat(name, attributes);
    }

    LOG_DBG("instantiating %s", name);

    FcPattern *pattern = FcNameParse((const unsigned char *)name);
    if (pattern == NULL) {
        LOG_ERR("%s: failed to lookup font", name);
        return false;
    }

    if (!FcConfigSubstitute(NULL, pattern, FcMatchPattern)) {
        LOG_ERR("%s: failed to do config substitution", name);
        FcPatternDestroy(pattern);
        return false;
    }

    FcDefaultSubstitute(pattern);

    FcResult result;
    FcFontSet *fonts = FcFontSort(NULL, pattern, FcTrue, NULL, &result);
    if (result != FcResultMatch) {
        LOG_ERR("%s: failed to match font", name);
        FcPatternDestroy(pattern);
        return false;
    }

    if (!from_font_set(pattern, fonts, 0, fallbacks, attributes, font, is_fallback)) {
        FcFontSetDestroy(fonts);
        FcPatternDestroy(pattern);
        return false;
    }

    if (is_fallback) {
        FcFontSetDestroy(fonts);
        FcPatternDestroy(pattern);
    }

    return true;
}

bool
font_from_name(font_list_t names, const char *attributes, struct font *font)
{
    if (tll_length(names) == 0)
        return false;

    font_list_t fallbacks = tll_init();
    bool skip_first = true;
    tll_foreach(names, it) {
        if (skip_first) {
            skip_first = false;
            continue;
        }

        tll_push_back(fallbacks, it->item);
    }

    bool ret = from_name(tll_front(names), &fallbacks, attributes, font, false);

    tll_free(fallbacks);
    return ret;
}

static size_t
hash_index(wchar_t wc)
{
    return wc % cache_size;
}

static bool
glyph_for_wchar(struct font *font, wchar_t wc, struct glyph *glyph)
{
    /*
     * LCD filter is per library instance. Thus we need to re-set it
     * every time...
     *
     * Also note that many freetype builds lack this feature
     * (FT_CONFIG_OPTION_SUBPIXEL_RENDERING must be defined, and isn't
     * by default) */
    FT_Error err = FT_Library_SetLcdFilter(ft_lib, font->lcd_filter);
    if (err != 0 && err != FT_Err_Unimplemented_Feature)
        goto err;

    FT_UInt idx = FT_Get_Char_Index(font->face, wc);
    if (idx == 0) {
        /* No glyph in this font, try fallback fonts */
        struct font fallback;

        /* Try user configured fallback fonts */
        tll_foreach(font->fallbacks, it) {
            if (from_name(it->item, NULL, "", &fallback, true)) {
                if (glyph_for_wchar(&fallback, wc, glyph)) {
                    LOG_DBG("%C: used fallback %s (fixup = %f)",
                            wc, it->item, fallback.pixel_size_fixup);
                    font_destroy(&fallback);
                    return true;
                }

                font_destroy(&fallback);
            }
        }

        if (font->is_fallback)
            return false;

        /* Try fontconfig fallback fonts */

        assert(font->fc_pattern != NULL);
        assert(font->fc_fonts != NULL);
        assert(font->fc_idx != -1);

        for (int i = font->fc_idx + 1; i < font->fc_fonts->nfont; i++) {
            if (from_font_set(font->fc_pattern, font->fc_fonts, i, NULL, "", &fallback, true)) {
                if (glyph_for_wchar(&fallback, wc, glyph)) {
                    LOG_DBG("%C: used fontconfig fallback", wc);
                    font_destroy(&fallback);
                    return true;
                }

                font_destroy(&fallback);
            }
        }

        LOG_WARN("%C: no glyph found (in neither the main font, "
                 "nor any fallback fonts)", wc);
    }

    err = FT_Load_Glyph(font->face, idx, font->load_flags);
    if (err != 0) {
        LOG_ERR("load failed");
        goto err;
    }

    err = FT_Render_Glyph(font->face->glyph, font->render_flags);
    if (err != 0)
        goto err;

    assert(font->face->glyph->format == FT_GLYPH_FORMAT_BITMAP);

    FT_Bitmap *bitmap = &font->face->glyph->bitmap;
    if (bitmap->width == 0)
        goto err;

    pixman_format_code_t pix_format;
    int width;
    int rows;

    switch (bitmap->pixel_mode) {
    case FT_PIXEL_MODE_MONO:
        pix_format = PIXMAN_a1;
        width = bitmap->width;
        rows = bitmap->rows;
        break;

    case FT_PIXEL_MODE_GRAY:
        pix_format = PIXMAN_a8;
        width = bitmap->width;
        rows = bitmap->rows;
        break;

    case FT_PIXEL_MODE_LCD:
        pix_format = PIXMAN_x8r8g8b8;
        width = bitmap->width / 3;
        rows = bitmap->rows;
        break;

    case FT_PIXEL_MODE_LCD_V:
        pix_format = PIXMAN_x8r8g8b8;
        width = bitmap->width;
        rows = bitmap->rows / 3;
        break;

    case FT_PIXEL_MODE_BGRA:
        pix_format = PIXMAN_a8r8g8b8;
        width = bitmap->width;
        rows = bitmap->rows;
        break;

    default:
        LOG_ERR("unimplemented: FT pixel mode: %d", bitmap->pixel_mode);
        goto err;
        break;
    }

    int stride = stride_for_format_and_width(pix_format, width);
    assert(stride >= bitmap->pitch);

    uint8_t *data = malloc(rows * stride);

    /* Convert FT bitmap to pixman image */
    switch (bitmap->pixel_mode) {
    case FT_PIXEL_MODE_MONO:
        for (size_t r = 0; r < bitmap->rows; r++) {
            for (size_t c = 0; c < (bitmap->width + 7) / 8; c++) {
                uint8_t v = bitmap->buffer[r * bitmap->pitch + c];
                uint8_t reversed = 0;
                for (size_t i = 0; i < min(8, bitmap->width - c * 8); i++)
                    reversed |= ((v >> (7 - i)) & 1) << i;

                data[r * stride + c] = reversed;
            }
        }
        break;

    case FT_PIXEL_MODE_GRAY:
        for (size_t r = 0; r < bitmap->rows; r++) {
            for (size_t c = 0; c < bitmap->width; c++)
                data[r * stride + c] = bitmap->buffer[r * bitmap->pitch + c];
        }
        break;

    case FT_PIXEL_MODE_BGRA:
        assert(stride == bitmap->pitch);
        memcpy(data, bitmap->buffer, bitmap->rows * bitmap->pitch);
        break;

    case FT_PIXEL_MODE_LCD:
        for (size_t r = 0; r < bitmap->rows; r++) {
            for (size_t c = 0; c < bitmap->width; c += 3) {
                unsigned char _r = bitmap->buffer[r * bitmap->pitch + c + 0];
                unsigned char _g = bitmap->buffer[r * bitmap->pitch + c + 1];
                unsigned char _b = bitmap->buffer[r * bitmap->pitch + c + 2];

                uint32_t *p = (uint32_t *)&data[r * stride + 4 * (c / 3)];
                *p =  _r << 16 | _g << 8 | _b;
            }
        }
        break;

    default:
        abort();
        break;
    }

    pixman_image_t *pix = pixman_image_create_bits_no_clear(
        pix_format, width, rows, (uint32_t *)data, stride);

    if (pix == NULL) {
        free(data);
        goto err;
    }

    pixman_image_set_component_alpha(
        pix,
        bitmap->pixel_mode == FT_PIXEL_MODE_LCD ||
        bitmap->pixel_mode == FT_PIXEL_MODE_LCD_V);

    if (font->pixel_size_fixup != 1.) {
        struct pixman_transform scale;
        pixman_transform_init_scale(
            &scale,
            pixman_double_to_fixed(1. / font->pixel_size_fixup),
            pixman_double_to_fixed(1. / font->pixel_size_fixup));
        pixman_image_set_transform(pix, &scale);
    }

    *glyph = (struct glyph){
        .wc = wc,
        .cols = wcwidth(wc),
        .pix = pix,
        .x = font->face->glyph->bitmap_left / font->pixel_size_fixup,
        .y = font->face->glyph->bitmap_top * font->pixel_size_fixup,
        .width = width,
        .height = rows,
        .valid = true,
    };

    return true;

err:
    *glyph = (struct glyph){
        .wc = wc,
        .valid = false,
    };
    return false;
}

const struct glyph *
font_glyph_for_wc(struct font *font, wchar_t wc)
{
    mtx_lock(&font->lock);

    assert(font->cache != NULL);
    size_t hash_idx = hash_index(wc);
    hash_entry_t *hash_entry = font->cache[hash_idx];

    if (hash_entry != NULL) {
        tll_foreach(*hash_entry, it) {
            if (it->item.wc == wc) {
                mtx_unlock(&font->lock);
                return it->item.valid ? &it->item : NULL;
            }
        }
    }

    struct glyph glyph;
    bool got_glyph = glyph_for_wchar(font, wc, &glyph);

    if (hash_entry == NULL) {
        hash_entry = calloc(1, sizeof(*hash_entry));

        assert(font->cache[hash_idx] == NULL);
        font->cache[hash_idx] = hash_entry;
    }

    assert(hash_entry != NULL);
    tll_push_back(*hash_entry, glyph);

    mtx_unlock(&font->lock);
    return got_glyph ? &tll_back(*hash_entry) : NULL;
}

void
font_destroy(struct font *font)
{
    tll_free_and_free(font->fallbacks, free);

    if (font->face != NULL) {
        mtx_lock(&ft_lock);
        FT_Done_Face(font->face);
        mtx_unlock(&ft_lock);
    }

    mtx_destroy(&font->lock);

    if (font->fc_pattern != NULL)
        FcPatternDestroy(font->fc_pattern);
    if (font->fc_fonts != NULL)
        FcFontSetDestroy(font->fc_fonts);

    if (font->cache == NULL)
        return;

    for (size_t i = 0; i < cache_size; i++) {
        if (font->cache[i] == NULL)
            continue;

        tll_foreach(*font->cache[i], it) {
            if (!it->item.valid)
                continue;

            void *image = pixman_image_get_data(it->item.pix);
            pixman_image_unref(it->item.pix);
            free(image);
        }

        tll_free(*font->cache[i]);
        free(font->cache[i]);
    }
    free(font->cache);
}
