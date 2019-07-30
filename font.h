#pragma once

#include <stdbool.h>
#include <threads.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include <cairo.h>

#include "tllist.h"
//#include "terminal.h"

typedef tll(const char *) font_list_t;

struct glyph {
    wchar_t wc;

    void *data;
    cairo_surface_t *surf;
    int left;
    int top;

#if 0
    int format;
    int width;
    int height;
    int stride;
#endif
};

typedef tll(struct glyph) hash_entry_t;

struct font {
    FT_Face face;
    int load_flags;
    int render_flags;
    FT_LcdFilter lcd_filter;
    struct {
        double position;
        double thickness;
    } underline;
    struct {
        double position;
        double thickness;
    } strikeout;

    bool is_fallback;
    tll(char *) fallbacks;

    //struct glyph cache[256];
    hash_entry_t **cache;
    mtx_t lock;
};

bool font_from_name(font_list_t names, const char *attributes, struct font *result);
const struct glyph *font_glyph_for_utf8(struct font *font, const char *utf8);
void font_destroy(struct font *font);
