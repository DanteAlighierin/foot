#pragma once

#include <fcft/fcft.h>

struct terminal;
struct fcft_glyph *box_drawing(struct terminal *term, wchar_t wc);
