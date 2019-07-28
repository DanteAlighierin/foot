#include "font.h"

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#include <fontconfig/fontconfig.h>
#include <cairo-ft.h>

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

    cairo_font_options_t *options = cairo_font_options_create();
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_DEFAULT);
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_DEFAULT);
    cairo_font_options_set_subpixel_order(options, CAIRO_SUBPIXEL_ORDER_DEFAULT);
    cairo_ft_font_options_substitute(options, pattern);

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

    cairo_font_options_set_hint_style(
        options, fc_hinting ? CAIRO_HINT_STYLE_DEFAULT : CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_antialias(
        options, fc_antialias ? CAIRO_ANTIALIAS_DEFAULT : CAIRO_ANTIALIAS_NONE);

    cairo_font_face_t *face = cairo_ft_font_face_create_for_pattern(
        final_pattern);

    FcPatternDestroy(final_pattern);

    if (cairo_font_face_status(face) != CAIRO_STATUS_SUCCESS) {
        LOG_ERR("%s: failed to create cairo font face", name);
        cairo_font_face_destroy(face);
        return false;
    }

    cairo_matrix_t matrix, ctm;
    cairo_matrix_init_identity(&ctm);
    cairo_matrix_init_scale(&matrix, size, size);

    cairo_scaled_font_t *scaled_font = cairo_scaled_font_create(
        face, &matrix, &ctm, options);

    cairo_font_options_destroy(options);
    cairo_font_face_destroy(face);

    if (cairo_scaled_font_status(scaled_font) != CAIRO_STATUS_SUCCESS) {
        LOG_ERR("%s: failed to create scaled font", name);
        cairo_scaled_font_destroy(scaled_font);
        return false;
    }

    font->face = ft_face;
    font->font = scaled_font;
    return true;
}
