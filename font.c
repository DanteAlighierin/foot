#include "font.h"

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#include <fontconfig/fontconfig.h>

#define LOG_MODULE "font"
#include "log.h"

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

    /* TODO: use FT_Set_Char_Size() if FC_PIXEL_SIZE doesn't exist, and use size instead? */
    if ((ft_err = FT_Set_Pixel_Sizes(ft_face, 0, size)) != 0)
        LOG_WARN("failed to set FreeType pixel sizes");

    FcBool fc_hinting, fc_antialias;
    if (FcPatternGetBool(final_pattern, FC_HINTING,0,  &fc_hinting) != FcResultMatch)
        fc_hinting = FcTrue;

    if (FcPatternGetBool(final_pattern, FC_ANTIALIAS, 0, &fc_antialias) != FcResultMatch)
        fc_antialias = FcTrue;

    FcPatternDestroy(final_pattern);

    font->face = ft_face;
    font_populate_glyph_cache(font);
    return true;
}

bool
font_glyph_for_utf8(const struct font *font, const char *utf8,
                    struct glyph *glyph)
{
    wchar_t wc;
    if (mbstowcs(&wc, utf8, 1) < 0)
        return false;

    FT_UInt idx = FT_Get_Char_Index(font->face, wc);
    FT_Error err = FT_Load_Glyph(font->face, idx, FT_LOAD_DEFAULT);
    if (err != 0)
        return false;

    err = FT_Render_Glyph(font->face->glyph, FT_RENDER_MODE_NORMAL);
    if (err != 0)
        return false;

    assert(font->face->glyph->format == FT_GLYPH_FORMAT_BITMAP);

    FT_Bitmap *bitmap = &font->face->glyph->bitmap;
    assert(bitmap->pixel_mode == FT_PIXEL_MODE_GRAY ||
           bitmap->pixel_mode == FT_PIXEL_MODE_MONO);

    cairo_format_t cr_format = bitmap->pixel_mode == FT_PIXEL_MODE_GRAY
        ? CAIRO_FORMAT_A8 : CAIRO_FORMAT_A1;

    int stride = cairo_format_stride_for_width(cr_format, bitmap->width);
    assert(stride >= bitmap->pitch);

    uint8_t *data = malloc(bitmap->rows * stride);

    switch (bitmap->pixel_mode) {
    case FT_PIXEL_MODE_MONO:
        for (size_t r = 0; r < bitmap->rows; r++) {
            for (size_t c = 0; c < bitmap->width; c++) {
                uint8_t v = bitmap->buffer[r * bitmap->pitch + c];
                uint8_t reversed = 0;
                for (size_t i = 0; i < 8; i++) {
                    reversed |= (v & 1) << (7 - i);
                    v >>= 1;
                }
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
        return false;
    }

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        data, cr_format, bitmap->width, bitmap->rows, stride);

    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        free(data);
        cairo_surface_destroy(surf);
        return false;
    }

    *glyph = (struct glyph){
        .data = data,
        .surf = surf,
        .left = font->face->glyph->bitmap_left,
        .top = font->face->glyph->bitmap_top,
    };
    return true;
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
}
