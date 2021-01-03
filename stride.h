#pragma once

#include <pixman.h>

static inline int
stride_for_format_and_width(pixman_format_code_t format, int width)
{
    return (((PIXMAN_FORMAT_BPP(format) * width + 7) / 8 + 4 - 1) & -4);
}
