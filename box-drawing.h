#pragma once

#include <uchar.h>
#include <fcft/fcft.h>

struct terminal;
struct fcft_glyph *box_drawing(const struct terminal *term, char32_t wc);
