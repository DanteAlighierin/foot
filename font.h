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

struct font {
    mtx_t lock;
    FT_Face face;
    int load_flags;
    int render_flags;
    FT_LcdFilter lcd_filter;

    double pixel_size_fixup; /* Scale factor - should only be used with ARGB32 glyphs */
    bool bgr;  /* True for FC_RGBA_BGR and FC_RGBA_VBGR */

    struct {
        int position;
        int thickness;
    } underline;

    struct {
        int position;
        int thickness;
    } strikeout;

    bool is_fallback;
    tll(char *) fallbacks;

    /* Fields below are only valid for non-fallback fonts */
    FcPattern *fc_pattern;
    FcFontSet *fc_fonts;
    int fc_idx;
    struct font **fc_loaded_fonts; /* fc_fonts->nfont array */

    hash_entry_t **cache;
};

struct font *font_from_name(font_list_t names, const char *attributes);
const struct glyph *font_glyph_for_wc(struct font *font, wchar_t wc);
void font_destroy(struct font *font);
