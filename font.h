#pragma once

#include <stdbool.h>
#include "terminal.h"

bool font_from_name(const char *name, struct font *result);
bool font_glyph_for_utf8(
    const struct font *font, const char *utf8, struct glyph *glyph);
void font_destroy(struct font *font);
