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
    FcPattern *fc_pattern;
    FcFontSet *fc_fonts;
    int fc_idx;

    FT_Face face;
    int load_flags;
    int render_flags;
    FT_LcdFilter lcd_filter;
    double pixel_size_fixup; /* Scale factor - should only be used with ARGB32 glyphs */

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

    hash_entry_t **cache;
    mtx_t lock;
};

bool font_from_name(font_list_t names, const char *attributes, struct font *result);
const struct glyph *font_glyph_for_wc(struct font *font, wchar_t wc);
void font_destroy(struct font *font);
