#pragma once

#include <stdbool.h>
#include <threads.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include <fontconfig/fontconfig.h>
#include <pixman.h>

#include "tllist.h"
//#include "terminal.h"

typedef tll(const char *) font_list_t;

struct glyph {
    wchar_t wc;
    int cols;

    pixman_image_t *pix;
    int x;
    int y;
    int width;
    int height;

    bool valid;
};

typedef tll(struct glyph) hash_entry_t;

struct font_fallback {
    char *pattern;
    struct font *font;
};

struct font {
    char *name;

    mtx_t lock;
    FT_Face face;
    int load_flags;
    int render_flags;
    FT_LcdFilter lcd_filter;

    double pixel_size_fixup; /* Scale factor - should only be used with ARGB32 glyphs */
    bool bgr;  /* True for FC_RGBA_BGR and FC_RGBA_VBGR */

    /* font extents */
    int height;
    int descent;
    int ascent;
    int max_x_advance;

    struct {
        double position;
        double thickness;
    } underline;

    struct {
        double position;
        double thickness;
    } strikeout;

    bool is_fallback;
    tll(struct font_fallback) fallbacks;

    size_t ref_counter;

    /* Fields below are only valid for non-fallback fonts */
    FcPattern *fc_pattern;
    FcFontSet *fc_fonts;
    int fc_idx;
    struct font **fc_loaded_fallbacks; /* fc_fonts->nfont array */

    hash_entry_t **glyph_cache;
};

struct font *font_from_name(font_list_t names, const char *attributes);
const struct glyph *font_glyph_for_wc(struct font *font, wchar_t wc);
void font_destroy(struct font *font);
