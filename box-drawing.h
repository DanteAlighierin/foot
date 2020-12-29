#pragma once

#include <fcft/fcft.h>

struct terminal;
struct fcft_glyph *box_drawing(const struct terminal *term, wchar_t wc);
