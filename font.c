#include "font.h"

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#include <fontconfig/fontconfig.h>
#include <cairo-ft.h>

#define LOG_MODULE "font"
#include "log.h"

static void __attribute__((constructor))
init(void)
{
    FcInit();
}

static void __attribute__((destructor))
fini(void)
{
    FcFini();
}
cairo_scaled_font_t *
font_from_name(const char *name)
{
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
        return NULL;
    }

    FcBool fc_hinting, fc_antialias;
    if (FcPatternGetBool(final_pattern, FC_HINTING,0,  &fc_hinting) != FcResultMatch)
        fc_hinting = FcTrue;

    if (FcPatternGetBool(final_pattern, FC_ANTIALIAS, 0, &fc_antialias) != FcResultMatch)
        fc_antialias = FcTrue;

    cairo_font_options_set_hint_style(
        options, fc_hinting ? CAIRO_HINT_STYLE_DEFAULT : CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_antialias(
        options, fc_antialias ? CAIRO_ANTIALIAS_DEFAULT : CAIRO_ANTIALIAS_NONE);

    double size;
    if (FcPatternGetDouble(final_pattern, FC_PIXEL_SIZE, 0, &size)) {
        LOG_ERR("%s: failed to get size", name);
        FcPatternDestroy(final_pattern);
        return NULL;
    }

    cairo_font_face_t *face = cairo_ft_font_face_create_for_pattern(
        final_pattern);

    FcPatternDestroy(final_pattern);

    if (cairo_font_face_status(face) != CAIRO_STATUS_SUCCESS) {
        LOG_ERR("%s: failed to create cairo font face", name);
        cairo_font_face_destroy(face);
        return NULL;
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
        return NULL;
    }

    return scaled_font;
}
