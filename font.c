#include "font.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include <math.h>
#include <assert.h>
#include <threads.h>

#include <freetype/tttables.h>

#define LOG_MODULE "font"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "stride.h"

#define min(x, y) ((x) < (y) ? (x) : (y))

static FT_Library ft_lib;
static mtx_t ft_lock;

static const size_t glyph_cache_size = 512;

struct font_cache_entry {
    uint64_t hash;
    struct font *font;
};

static tll(struct font_cache_entry) font_cache = tll_init();

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
    while (tll_length(font_cache) > 0)
        font_destroy(tll_pop_front(font_cache).font);

    mtx_destroy(&ft_lock);
    FT_Done_FreeType(ft_lib);
    FcFini();
}

static const char *
ft_error_string(FT_Error err)
{
    #undef FTERRORS_H_
    #undef __FTERRORS_H__
    #define FT_ERRORDEF( e, v, s )  case e: return s;
    #define FT_ERROR_START_LIST     switch (err) {
    #define FT_ERROR_END_LIST       }
    #include FT_ERRORS_H
    return "unknown error";
}
static void
underline_strikeout_metrics(struct font *font)
{
    FT_Face ft_face = font->face;
    double y_scale = ft_face->size->metrics.y_scale / 65536.;
    double height = ft_face->size->metrics.height / 64.;
    double descent = ft_face->size->metrics.descender / 64.;

    LOG_DBG("ft: y-scale: %f, height: %f, descent: %f",
            y_scale, height, descent);

    font->underline.position = ft_face->underline_position * y_scale / 64.;
    font->underline.thickness = ft_face->underline_thickness * y_scale / 64.;

    if (font->underline.position == 0.) {
        font->underline.position = descent / 2.;
        font->underline.thickness = fabs(descent / 5.);
    }

    LOG_DBG("underline: pos=%f, thick=%f",
            font->underline.position, font->underline.thickness);

    TT_OS2 *os2 = FT_Get_Sfnt_Table(ft_face, ft_sfnt_os2);
    if (os2 != NULL) {
        font->strikeout.position = os2->yStrikeoutPosition * y_scale / 64.;
        font->strikeout.thickness = os2->yStrikeoutSize * y_scale / 64.;
    }

    if (font->strikeout.position == 0.) {
        font->strikeout.position = height / 2. + descent;
        font->strikeout.thickness = font->underline.thickness;
    }

    LOG_DBG("strikeout: pos=%f, thick=%f",
            font->strikeout.position, font->strikeout.thickness);
}

static bool
from_font_set(FcPattern *pattern, FcFontSet *fonts, int start_idx,
              struct font *font, bool is_fallback)
{
    memset(font, 0, sizeof(*font));

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
        dpi = 75;

    double size;
    if (FcPatternGetDouble(final_pattern, FC_SIZE, 0, &size) != FcResultMatch)
        LOG_WARN("%s: failed to get size", face_file);

    double pixel_size;
    if (FcPatternGetDouble(final_pattern, FC_PIXEL_SIZE, 0, &pixel_size) != FcResultMatch) {
        LOG_ERR("%s: failed to get pizel size", face_file);
        FcPatternDestroy(final_pattern);
        return false;
    }

    mtx_lock(&ft_lock);
    FT_Face ft_face;
    FT_Error ft_err = FT_New_Face(ft_lib, (const char *)face_file, 0, &ft_face);
    mtx_unlock(&ft_lock);
    if (ft_err != 0) {
        LOG_ERR("%s: failed to create FreeType face", face_file);
        FcPatternDestroy(final_pattern);
        return false;
    }

    if ((ft_err = FT_Set_Pixel_Sizes(ft_face, 0, pixel_size)) != 0) {
        LOG_WARN("%s: failed to set character size", face_file);
        mtx_lock(&ft_lock);
        FT_Done_Face(ft_face);
        mtx_unlock(&ft_lock);
        FcPatternDestroy(final_pattern);
        return false;
    }

    FcBool scalable;
    if (FcPatternGetBool(final_pattern, FC_SCALABLE, 0, &scalable) != FcResultMatch)
        scalable = FcTrue;

    FcBool outline;
    if (FcPatternGetBool(final_pattern, FC_OUTLINE, 0, &outline) != FcResultMatch)
        outline = FcTrue;

    double pixel_fixup = 1.;
    if (FcPatternGetDouble(final_pattern, "pixelsizefixupfactor", 0, &pixel_fixup) != FcResultMatch) {
        /*
         * Force a fixup factor on scalable bitmap fonts (typically
         * emoji fonts). The fixup factor is
         *   requested-pixel-size / actual-pixels-size
         */
        if (scalable && !outline) {
            double requested_pixel_size;
            if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &requested_pixel_size) != FcResultMatch) {
                /* User didn't specify ":pixelsize=xy" */
                double requested_size;
                if (FcPatternGetDouble(pattern, FC_SIZE, 0, &requested_size) != FcResultMatch) {
                    /* User didn't specify ":size=xy" */
                    requested_size = size;
                }

                requested_pixel_size = size * dpi / 72;
            }

            pixel_fixup = requested_pixel_size / ft_face->size->metrics.y_ppem;
            LOG_DBG("estimated pixel fixup factor to %f (from pixel size: %f)",
                    pixel_fixup, requested_pixel_size);
        } else
            pixel_fixup = 1.;
    }

#if 0
    LOG_DBG("FIXED SIZES: %d", ft_face->num_fixed_sizes);
    for (int i = 0; i < ft_face->num_fixed_sizes; i++)
        LOG_DBG("  #%d: height=%d, y_ppem=%f", i, ft_face->available_sizes[i].height, ft_face->available_sizes[i].y_ppem / 64.);
#endif

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
        else if (fc_rgba == FC_RGBA_RGB || fc_rgba == FC_RGBA_BGR)
            load_flags |= FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD;
        else if (fc_rgba == FC_RGBA_VRGB || fc_rgba == FC_RGBA_VBGR)
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
        if (fc_rgba == FC_RGBA_RGB || fc_rgba == FC_RGBA_BGR)
            render_flags |= FT_RENDER_MODE_LCD;
        else if (fc_rgba == FC_RGBA_VRGB || fc_rgba == FC_RGBA_VBGR)
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

    font->name = strdup((char *)face_file);
    FcPatternDestroy(final_pattern);

    mtx_init(&font->lock, mtx_plain);
    font->face = ft_face;
    font->load_flags = load_flags | FT_LOAD_COLOR;
    font->render_flags = render_flags;
    font->is_fallback = is_fallback;
    font->pixel_size_fixup = pixel_fixup;
    font->bgr = fc_rgba == FC_RGBA_BGR || fc_rgba == FC_RGBA_VBGR;
    font->ref_counter = 1;
    font->fc_idx = font_idx;

    if (is_fallback) {
        font->fc_pattern = NULL;
        font->fc_fonts = NULL;
        font->fc_loaded_fallbacks = NULL;
        font->glyph_cache = NULL;
    } else {
        font->fc_pattern = !is_fallback ? pattern : NULL;
        font->fc_fonts = !is_fallback ? fonts : NULL;
        font->fc_loaded_fallbacks = calloc(
            fonts->nfont, sizeof(font->fc_loaded_fallbacks[0]));
        font->glyph_cache = calloc(glyph_cache_size, sizeof(font->glyph_cache[0]));
    }

    double max_x_advance = ft_face->size->metrics.max_advance / 64.;
    double height= ft_face->size->metrics.height / 64.;
    double descent = ft_face->size->metrics.descender / 64.;
    double ascent = ft_face->size->metrics.ascender / 64.;

    font->height = ceil(height * font->pixel_size_fixup);
    font->descent = ceil(-descent * font->pixel_size_fixup);
    font->ascent = ceil(ascent * font->pixel_size_fixup);
    font->max_x_advance = ceil(max_x_advance * font->pixel_size_fixup);

    LOG_DBG("%s: size=%f, pixel-size=%f, dpi=%f, fixup-factor: %f, "
            "line-height: %d, ascent: %d, descent: %d, x-advance: %d",
            font->name, size, pixel_size, dpi, font->pixel_size_fixup,
            font->height, font->ascent, font->descent,
            font->max_x_advance);

    underline_strikeout_metrics(font);
    return true;
}

static struct font *
from_name(const char *name, bool is_fallback)
{
    LOG_DBG("instantiating %s", name);

    FcPattern *pattern = FcNameParse((const unsigned char *)name);
    if (pattern == NULL) {
        LOG_ERR("%s: failed to lookup font", name);
        return NULL;
    }

    if (!FcConfigSubstitute(NULL, pattern, FcMatchPattern)) {
        LOG_ERR("%s: failed to do config substitution", name);
        FcPatternDestroy(pattern);
        return NULL;
    }

    FcDefaultSubstitute(pattern);

    FcResult result;
    FcFontSet *fonts = FcFontSort(NULL, pattern, FcTrue, NULL, &result);
    if (result != FcResultMatch) {
        LOG_ERR("%s: failed to match font", name);
        FcPatternDestroy(pattern);
        return NULL;
    }

    struct font *font = malloc(sizeof(*font));

    if (!from_font_set(pattern, fonts, 0, font, is_fallback)) {
        free(font);
        FcFontSetDestroy(fonts);
        FcPatternDestroy(pattern);
        return NULL;
    }

    if (is_fallback) {
        FcFontSetDestroy(fonts);
        FcPatternDestroy(pattern);
    }

    return font;
}

static uint64_t
sdbm_hash(const char *s)
{
    uint64_t hash = 0;

    for (; *s != '\0'; s++) {
        int c = *s;
        hash = c + (hash << 6) + (hash << 16) - hash;
    }

    return hash;
}

static uint64_t
font_hash(font_list_t names, const char *attributes)
{
    uint64_t hash = 0;
    tll_foreach(names, it)
        hash ^= sdbm_hash(it->item);

    if (attributes != NULL)
        hash ^= sdbm_hash(attributes);

    return hash;
}

struct font *
font_from_name(font_list_t names, const char *attributes)
{
    if (tll_length(names) == 0)
        return false;

    uint64_t hash = font_hash(names, attributes);
    tll_foreach(font_cache, it) {
        if (it->item.hash == hash) {
            it->item.font->ref_counter++;
            return it->item.font;
        }
    }

    struct font *font = NULL;

    bool have_attrs = attributes != NULL && strlen(attributes) > 0;
    size_t attr_len = have_attrs ? strlen(attributes) + 1 : 0;

    bool first = true;
    tll_foreach(names, it) {
        const char *base_name = it->item;

        char name[strlen(base_name) + attr_len + 1];
        strcpy(name, base_name);
        if (have_attrs) {
            strcat(name, ":");
            strcat(name, attributes);
        }

        if (first) {
            first = false;

            font = from_name(name, false);
            if (font == NULL)
                return NULL;

            continue;
        }

        assert(font != NULL);
        tll_push_back(
            font->fallbacks, ((struct font_fallback){.pattern = strdup(name)}));
    }

    tll_push_back(font_cache, ((struct font_cache_entry){.hash = hash, .font = font}));
    return font;
}

static size_t
hash_index(wchar_t wc)
{
    return wc % glyph_cache_size;
}

static bool
glyph_for_wchar(const struct font *font, wchar_t wc, struct glyph *glyph)
{
    *glyph = (struct glyph){
        .wc = wc,
        .valid = false,
    };

    /*
     * LCD filter is per library instance. Thus we need to re-set it
     * every time...
     *
     * Also note that many freetype builds lack this feature
     * (FT_CONFIG_OPTION_SUBPIXEL_RENDERING must be defined, and isn't
     * by default) */
    FT_Error err = FT_Library_SetLcdFilter(ft_lib, font->lcd_filter);
    if (err != 0 && err != FT_Err_Unimplemented_Feature) {
        LOG_ERR("failed to set LCD filter: %s", ft_error_string(err));
        goto err;
    }

    FT_UInt idx = FT_Get_Char_Index(font->face, wc);
    if (idx == 0) {
        /* No glyph in this font, try fallback fonts */

        tll_foreach(font->fallbacks, it) {
            if (it->item.font == NULL) {
                it->item.font = from_name(it->item.pattern, true);
                if (it->item.font == NULL)
                    continue;
            }

            if (glyph_for_wchar(it->item.font, wc, glyph)) {
                LOG_DBG("%C: used fallback: %s", wc, it->item.font->name);
                return true;
            }
        }

        if (font->is_fallback)
            return false;

        /* Try fontconfig fallback fonts */

        assert(font->fc_pattern != NULL);
        assert(font->fc_fonts != NULL);
        assert(font->fc_loaded_fallbacks != NULL);
        assert(font->fc_idx != -1);

        for (int i = font->fc_idx + 1; i < font->fc_fonts->nfont; i++) {
            if (font->fc_loaded_fallbacks[i] == NULL) {
                /* Load font */
                struct font *fallback = malloc(sizeof(*fallback));
                if (!from_font_set(font->fc_pattern, font->fc_fonts, i, fallback, true))
                {
                    LOG_WARN("failed to load fontconfig fallback font");
                    free(fallback);
                    continue;
                }

                LOG_DBG("loaded new fontconfig fallback font");
                assert(fallback->fc_idx >= i);

                i = fallback->fc_idx;
                font->fc_loaded_fallbacks[i] = fallback;
            }

            assert(font->fc_loaded_fallbacks[i] != NULL);

            if (glyph_for_wchar(font->fc_loaded_fallbacks[i], wc, glyph)) {
                LOG_DBG("%C: used fontconfig fallback: %s",
                        wc, font->fc_loaded_fallbacks[i]->name);
                return true;
            }
        }

        LOG_DBG("%C: no glyph found (in neither the main font, "
                "nor any fallback fonts)", wc);
    }

    err = FT_Load_Glyph(font->face, idx, font->load_flags);
    if (err != 0) {
        LOG_ERR("%s: failed to load glyph #%d: %s",
                font->name, idx, ft_error_string(err));
        goto err;
    }

    err = FT_Render_Glyph(font->face->glyph, font->render_flags);
    if (err != 0) {
        LOG_ERR("%s: failed to render glyph: %s", font->name, ft_error_string(err));
        goto err;
    }

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
                unsigned char _r = bitmap->buffer[r * bitmap->pitch + c + (font->bgr ? 2 : 0)];
                unsigned char _g = bitmap->buffer[r * bitmap->pitch + c + 1];
                unsigned char _b = bitmap->buffer[r * bitmap->pitch + c + (font->bgr ? 0 : 2)];

                uint32_t *p = (uint32_t *)&data[r * stride + 4 * (c / 3)];
                *p =  _r << 16 | _g << 8 | _b;
            }
        }
        break;

    case FT_PIXEL_MODE_LCD_V:
        /* Unverified */
        for (size_t r = 0; r < bitmap->rows; r += 3) {
            for (size_t c = 0; c < bitmap->width; c++) {
                unsigned char _r = bitmap->buffer[(r + (font->bgr ? 2 : 0)) * bitmap->pitch + c];
                unsigned char _g = bitmap->buffer[(r + 1) * bitmap->pitch + c];
                unsigned char _b = bitmap->buffer[(r + (font->bgr ? 0 : 2)) * bitmap->pitch + c];

                uint32_t *p = (uint32_t *)&data[r / 3 * stride + 4 * c];
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
        pixman_transform_init_identity(&scale);
        pixman_transform_scale(
            &scale, NULL,
            pixman_double_to_fixed(1.0 / font->pixel_size_fixup),
            pixman_double_to_fixed(1.0 / font->pixel_size_fixup));
        pixman_image_set_transform(pix, &scale);
        pixman_image_set_filter(pix, PIXMAN_FILTER_BEST, NULL, 0);
    }

    int cols = wcwidth(wc);
    if (cols < 0)
        cols = 0;

    *glyph = (struct glyph){
        .wc = wc,
        .cols = cols,
        .pix = pix,
        .x = font->face->glyph->bitmap_left * font->pixel_size_fixup,
        .y = font->face->glyph->bitmap_top * font->pixel_size_fixup,
        .width = width,
        .height = rows,
        .valid = true,
    };

    return true;

err:
    assert(!glyph->valid);
    return false;
}

const struct glyph *
font_glyph_for_wc(struct font *font, wchar_t wc)
{
    mtx_lock(&font->lock);

    assert(font->glyph_cache != NULL);
    size_t hash_idx = hash_index(wc);
    hash_entry_t *hash_entry = font->glyph_cache[hash_idx];

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

        assert(font->glyph_cache[hash_idx] == NULL);
        font->glyph_cache[hash_idx] = hash_entry;
    }

    assert(hash_entry != NULL);
    tll_push_back(*hash_entry, glyph);

    mtx_unlock(&font->lock);
    return got_glyph ? &tll_back(*hash_entry) : NULL;
}

void
font_destroy(struct font *font)
{
    if (font == NULL)
        return;

    if (--font->ref_counter > 0)
        return;

    tll_foreach(font_cache, it) {
        if (it->item.font == font) {
            tll_remove(font_cache, it);
            break;
        }
    }

    free(font->name);

    tll_foreach(font->fallbacks, it) {
        font_destroy(it->item.font);
        free(it->item.pattern);
    }
    tll_free(font->fallbacks);

    if (font->face != NULL) {
        mtx_lock(&ft_lock);
        FT_Done_Face(font->face);
        mtx_unlock(&ft_lock);
    }

    mtx_destroy(&font->lock);

    if (font->fc_fonts != NULL) {
        assert(font->fc_loaded_fallbacks != NULL);

        for (size_t i = 0; i < font->fc_fonts->nfont; i++)
            font_destroy(font->fc_loaded_fallbacks[i]);

        free(font->fc_loaded_fallbacks);
    }

    if (font->fc_pattern != NULL)
        FcPatternDestroy(font->fc_pattern);
    if (font->fc_fonts != NULL)
        FcFontSetDestroy(font->fc_fonts);


    for (size_t i = 0; i < glyph_cache_size && font->glyph_cache != NULL; i++) {
        if (font->glyph_cache[i] == NULL)
            continue;

        tll_foreach(*font->glyph_cache[i], it) {
            if (!it->item.valid)
                continue;

            void *image = pixman_image_get_data(it->item.pix);
            pixman_image_unref(it->item.pix);
            free(image);
        }

        tll_free(*font->glyph_cache[i]);
        free(font->glyph_cache[i]);
    }
    free(font->glyph_cache);
    free(font);
}
