#pragma once

#include <stdint.h>

void rgb_to_hsl(uint32_t rgb, int *hue, int *sat, int *lum);
uint32_t hsl_to_rgb(int hue, int sat, int lum);
