#include "font.h"

#include <stdlib.h>
#include <stdbool.h>
#include <wchar.h>
#include <assert.h>

#include <fontconfig/fontconfig.h>

#define LOG_MODULE "font"
#include "log.h"

#define min(x, y) ((x) < (y) ? (x) : (y))

static FT_Library ft_lib;

static void __attribute__((constructor))
init(void)
{
    FcInit();
    FT_Init_FreeType(&ft_lib);
}

static void __attribute__((destructor))
fini(void)
{
    FcFini();
    FT_Done_FreeType(ft_lib);
}

static void
font_populate_glyph_cache(struct font *font)
{
    memset(font->cache, 0, sizeof(font->cache));
    for (size_t i = 0; i < 256; i++)
        font_glyph_for_utf8(font, &(char){i}, &font->cache[i]);
}

bool
font_from_name(const char *name, struct font *font)
{
    memset(font, 0, sizeof(*font));

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
    FcPattern *final_pattern = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);

    if (final_pattern == NULL) {
        LOG_ERR("%s: failed to match font", name);
        return false;
    }

    FcChar8 *face_file = NULL;
    if (FcPatternGetString(final_pattern, FC_FT_FACE, 0, &face_file) != FcResultMatch) {
        if (FcPatternGetString(final_pattern, FC_FILE, 0, &face_file) != FcResultMatch) {
            LOG_ERR("no font file name available");
            FcPatternDestroy(final_pattern);
            return false;
        }
    }

    double dpi;
    if (FcPatternGetDouble(final_pattern, FC_DPI, 0, &dpi) != FcResultMatch)
        dpi = 96;

    double size;
    if (FcPatternGetDouble(final_pattern, FC_PIXEL_SIZE, 0, &size)) {
        LOG_ERR("%s: failed to get size", name);
        FcPatternDestroy(final_pattern);
        return false;
    }

    LOG_DBG("loading: %s", face_file);

    FT_Face ft_face;
    FT_Error ft_err = FT_New_Face(ft_lib, (const char *)face_file, 0, &ft_face);
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
        else if (fc_rgba == FC_RGBA_RGB) {
            LOG_WARN("unimplemented: subpixel antialiasing");
            // load_flags |= FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD;
            load_flags |= FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL;
        } else if (fc_rgba == FC_RGBA_VRGB) {
            LOG_WARN("unimplemented: subpixel antialiasing");
            //load_flags |= FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD_V;
            load_flags |= FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL;
        } else
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
        if (false)
            ;
#if 0
        if (fc_rgba == FC_RGBA_RGB)
            render_flags |= FT_RENDER_MODE_LCD;
        else if (fc_rgba == FC_RGBA_VRGB)
            render_flags |= FT_RENDER_MODE_LCD_V;
#endif
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
    font->load_flags = load_flags;
    font->render_flags = render_flags;
    font_populate_glyph_cache(font);
    return true;
}

bool
font_glyph_for_utf8(struct font *font, const char *utf8,
                    struct glyph *glyph)
{
    mbstate_t ps = {0};
    wchar_t wc;
    if (mbrtowc(&wc, utf8, 4, &ps) < 0) {
        LOG_ERR("FAILED: %.4s", utf8);
        return false;
    }

    wprintf(L"CONVERTED: %.1s\n", &wc);

    mtx_lock(&font->lock);

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
    assert(bitmap->pixel_mode == FT_PIXEL_MODE_GRAY ||
           bitmap->pixel_mode == FT_PIXEL_MODE_MONO);

    cairo_format_t cr_format = bitmap->pixel_mode == FT_PIXEL_MODE_GRAY
        ? CAIRO_FORMAT_A8 : CAIRO_FORMAT_A1;

    int stride = cairo_format_stride_for_width(cr_format, bitmap->width);
    assert(stride >= bitmap->pitch);

    uint8_t *data = malloc(bitmap->rows * stride);
    assert(bitmap->pitch >= 0);

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

    default:
        LOG_ERR("unimplemented FreeType bitmap pixel mode: %d",
                bitmap->pixel_mode);
        free(data);
        goto err;
    }

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        data, cr_format, bitmap->width, bitmap->rows, stride);

    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        free(data);
        cairo_surface_destroy(surf);
        goto err;
    }

    *glyph = (struct glyph){
        .data = data,
        .surf = surf,
        .left = font->face->glyph->bitmap_left,
        .top = font->face->glyph->bitmap_top,

        .format = cr_format,
        .width = bitmap->width,
        .height = bitmap->rows,
        .stride = stride,
    };
    mtx_unlock(&font->lock);
    return true;

err:
    mtx_unlock(&font->lock);
    return false;
}

void
font_destroy(struct font *font)
{
    if (font->face != NULL)
        FT_Done_Face(font->face);

    for (size_t i = 0; i < 256; i++) {
        if (font->cache[i].surf != NULL)
            cairo_surface_destroy(font->cache[i].surf);
        if (font->cache[i].data != NULL)
            free(font->cache[i].data);
    }

    mtx_destroy(&font->lock);
}
