#include "hsl.h"

#include <math.h>

#include "util.h"

void
rgb_to_hsl(uint32_t rgb, int *hue, int *sat, int *lum)
{
    double r = (double)((rgb >> 16) & 0xff) / 255.;
    double g = (double)((rgb >> 8) & 0xff) / 255.;
    double b = (double)((rgb >> 0) & 0xff) / 255.;

    double x_max = max(max(r, g), b);
    double x_min = min(min(r, g), b);
    double V = x_max;

    double C = x_max - x_min;
    double L = (x_max + x_min) / 2.;

    *lum = 100 * L;

    if (C == 0.0)
        *hue = 0;
    else if (V == r)
        *hue = 60. * (0. + (g - b) / C);
    else if (V == g)
        *hue = 60. * (2. + (b - r) / C);
    else if (V == b)
        *hue = 60. * (4. + (r - g) / C);
    if (*hue < 0)
        *hue += 360;

    double S = C == 0.0
        ? 0
        : C / (1. - fabs(2. * L - 1.));
    *sat = 100 * S;
}

uint32_t
hsl_to_rgb(int hue, int sat, int lum)
{
    double L = lum / 100.0;
    double S = sat / 100.0;
    double C = (1. - fabs(2. * L - 1.)) * S;

    double X = C * (1. - fabs(fmod((double)hue / 60., 2.) - 1.));
    double m = L - C / 2.;

    double r, g, b;
    if (hue >= 0 && hue <= 60) {
        r = C;
        g = X;
        b = 0.;
    } else if (hue >= 60 && hue <= 120) {
        r = X;
        g = C;
        b = 0.;
    } else if (hue >= 120 && hue <= 180) {
        r = 0.;
        g = C;
        b = X;
    } else if (hue >= 180 && hue <= 240) {
        r = 0.;
        g = X;
        b = C;
    } else if (hue >= 240 && hue <= 300) {
        r = X;
        g = 0.;
        b = C;
    } else if (hue >= 300 && hue <= 360) {
        r = C;
        g = 0.;
        b = X;
    } else {
        r = 0.;
        g = 0.;
        b = 0.;
    }

    r += m;
    g += m;
    b += m;

    return (
        (int)round(r * 255.) << 16 |
        (int)round(g * 255.) << 8 |
        (int)round(b * 255.) << 0);
}
