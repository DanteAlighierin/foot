#include "box-drawing.h"

#include <stdio.h>
#include <errno.h>

#define LOG_MODULE "box-drawing"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "macros.h"
#include "stride.h"
#include "terminal.h"
#include "xmalloc.h"

#define LIGHT 1.0
#define HEAVY 2.0

static int CONST
thickness(float pts, int dpi)
{
    return pts * (float)dpi / 72.0;
}

static void
hline(uint8_t *buf, int x1, int x2, int y, int thick, int stride)
{
    for (size_t row = y; row < y + thick; row++) {
        for (size_t col = x1; col < x2; col++) {
            size_t idx = col / 8;
            size_t bit_no = col % 8;
            buf[row * stride + idx] |= 1 << bit_no;
        }
    }
}

static void
vline(uint8_t *buf, int y1, int y2, int x, int thick, int stride)
{
    for (size_t row = y1; row < y2; row++) {
        for (size_t col = x; col < x + thick; col++) {
            size_t idx = col / 8;
            size_t bit_no = col % 8;
            buf[row * stride + idx] |= 1 << bit_no;
        }
    }
}

#define _hline_middle_left(_vthick, _hthick)                            \
    do {                                                                \
        int vthick = thickness(_vthick, dpi);                           \
        int hthick = thickness(_hthick, dpi);                           \
        hline(buf, 0, (width + vthick) / 2, (height - hthick) / 2, hthick, stride); \
    } while (0)

#define _hline_middle_right(_vthick, _hthick)                           \
    do {                                                                \
        int vthick = thickness(_vthick, dpi);                           \
        int hthick = thickness(_hthick, dpi);                           \
        hline(buf, (width - vthick) / 2, width, (height - hthick) / 2, hthick, stride); \
    } while (0)

#define _vline_middle_up(_vthick, _hthick)                              \
    do {                                                                \
        int vthick = thickness(_vthick, dpi);                           \
        int hthick = thickness(_hthick, dpi);                           \
        vline(buf, 0, (height + hthick) / 2, (width - vthick) / 2, vthick, stride); \
    } while (0)

#define _vline_middle_down(_vthick, _hthick)                            \
    do {                                                                \
        int vthick = thickness(_vthick, dpi);                           \
        int hthick = thickness(_hthick, dpi);                           \
        vline(buf, (height - hthick) / 2, height, (width - vthick) / 2, vthick, stride); \
    } while (0)

#define hline_middle_left(thick) _hline_middle_left(thick, thick)
#define hline_middle_right(thick) _hline_middle_right(thick, thick)
#define vline_middle_up(thick) _vline_middle_up(thick, thick)
#define vline_middle_down(thick) _vline_middle_down(thick, thick)

static void
draw_box_drawings_light_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(LIGHT);
}

static void
draw_box_drawings_heavy_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(HEAVY);
}

static void
draw_box_drawings_light_vertical(uint8_t *buf, int width, int height, int stride, int dpi)
{
    vline_middle_up(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_heavy_vertical(uint8_t *buf, int width, int height, int stride, int dpi)
{
    vline_middle_up(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_light_down_and_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_down_light_and_right_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    _hline_middle_right(LIGHT, HEAVY);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_down_heavy_and_right_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(LIGHT);
    _vline_middle_down(HEAVY, LIGHT);
}

static void
draw_box_drawings_heavy_down_and_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_light_down_and_left(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_down_light_and_left_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    _hline_middle_left(LIGHT, HEAVY);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_down_heavy_and_left_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    _vline_middle_down(HEAVY, LIGHT);
}

static void
draw_box_drawings_heavy_down_and_left(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_light_up_and_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(LIGHT);
    vline_middle_up(LIGHT);
}

static void
draw_box_drawings_up_light_and_right_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    _hline_middle_right(LIGHT, HEAVY);
    vline_middle_up(LIGHT);
}

static void
draw_box_drawings_up_heavy_and_right_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(LIGHT);
    _vline_middle_up(HEAVY, LIGHT);
}

static void
draw_box_drawings_heavy_up_and_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(HEAVY);
    vline_middle_up(HEAVY);
}

static void
draw_box_drawings_light_up_and_left(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    vline_middle_up(LIGHT);
}

static void
draw_box_drawings_up_light_and_left_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    _hline_middle_left(LIGHT, HEAVY);
    vline_middle_up(LIGHT);
}

static void
draw_box_drawings_up_heavy_and_left_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    _vline_middle_up(HEAVY, LIGHT);
}

static void
draw_box_drawings_heavy_up_and_left(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    vline_middle_up(HEAVY);
}

static void
draw_box_drawings_light_vertical_and_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(LIGHT);
    vline_middle_up(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_vertical_light_and_right_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    _hline_middle_right(LIGHT, HEAVY);
    vline_middle_up(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_up_heavy_and_right_down_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(LIGHT);
    _vline_middle_up(HEAVY, LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_down_heavy_and_right_up_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(LIGHT);
    vline_middle_up(LIGHT);
    _vline_middle_down(HEAVY, LIGHT);
}

static void
draw_box_drawings_vertical_heavy_and_right_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(LIGHT);
    vline_middle_up(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_down_light_and_right_up_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(HEAVY);
    vline_middle_up(HEAVY);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_up_light_and_right_down_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(HEAVY);
    vline_middle_up(LIGHT);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_heavy_vertical_and_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(HEAVY);
    vline_middle_up(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_light_vertical_and_left(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    vline_middle_up(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_vertical_light_and_left_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    _hline_middle_left(LIGHT, HEAVY);
    vline_middle_up(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_up_heavy_and_left_down_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    _vline_middle_up(HEAVY, LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_down_heavy_and_left_up_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    vline_middle_up(LIGHT);
    _vline_middle_down(HEAVY, LIGHT);
}

static void
draw_box_drawings_vertical_heavy_and_left_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    vline_middle_up(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_down_light_and_left_up_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    vline_middle_up(HEAVY);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_up_light_and_left_down_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    vline_middle_up(LIGHT);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_heavy_vertical_and_left(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    vline_middle_up(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_light_down_and_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_left_heavy_and_right_down_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    _hline_middle_left(LIGHT, HEAVY);
    hline_middle_right(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_right_heavy_and_left_down_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    _hline_middle_right(LIGHT, HEAVY);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_down_light_and_horizontal_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(HEAVY);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_down_heavy_and_horizontal_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(LIGHT);
    _vline_middle_down(HEAVY, LIGHT);
}

static void
draw_box_drawings_right_light_and_left_down_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(LIGHT);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_left_light_and_right_down_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_heavy_down_and_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_light_up_and_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(LIGHT);
    vline_middle_up(LIGHT);
}

static void
draw_box_drawings_left_heavy_and_right_up_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    _hline_middle_left(LIGHT, HEAVY);
    hline_middle_right(LIGHT);
    vline_middle_up(LIGHT);
}

static void
draw_box_drawings_right_heavy_and_left_up_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    _hline_middle_right(LIGHT, HEAVY);
    vline_middle_up(LIGHT);
}

static void
draw_box_drawings_up_light_and_horizontal_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(HEAVY);
    vline_middle_up(LIGHT);
}

static void
draw_box_drawings_up_heavy_and_horizontal_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(LIGHT);
    _vline_middle_up(HEAVY, LIGHT);
}

static void
draw_box_drawings_right_light_and_left_up_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(LIGHT);
    vline_middle_up(HEAVY);
}

static void
draw_box_drawings_left_light_and_right_up_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(HEAVY);
    vline_middle_up(HEAVY);
}

static void
draw_box_drawings_heavy_up_and_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(HEAVY);
    vline_middle_up(HEAVY);
}

static void
draw_box_drawings_light_vertical_and_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(LIGHT);
    vline_middle_up(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_left_heavy_and_right_vertical_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    _hline_middle_left(LIGHT, HEAVY);
    hline_middle_right(LIGHT);
    vline_middle_up(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_right_heavy_and_left_vertical_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    _hline_middle_right(LIGHT, HEAVY);
    vline_middle_up(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_vertical_light_and_horizontal_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(HEAVY);
    vline_middle_up(LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_up_heavy_and_down_horizontal_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(LIGHT);
    _vline_middle_up(HEAVY, LIGHT);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_down_heavy_and_up_horizontal_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(LIGHT);
    vline_middle_up(LIGHT);
    _vline_middle_down(HEAVY, LIGHT);
}

static void
draw_box_drawings_vertical_heavy_and_horizontal_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(LIGHT);
    vline_middle_up(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_left_up_heavy_and_right_down_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(LIGHT);
    vline_middle_up(HEAVY);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_right_up_heavy_and_left_down_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(HEAVY);
    vline_middle_up(HEAVY);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_left_down_heavy_and_right_up_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(LIGHT);
    vline_middle_up(LIGHT);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_right_down_heavy_and_left_up_light(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(HEAVY);
    vline_middle_up(LIGHT);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_down_light_and_up_horizontal_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(HEAVY);
    vline_middle_up(HEAVY);
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_up_light_and_down_horizontal_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(HEAVY);
    vline_middle_up(LIGHT);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_right_light_and_left_vertical_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(LIGHT);
    vline_middle_up(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_left_light_and_right_vertical_heavy(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(HEAVY);
    vline_middle_up(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_heavy_vertical_and_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(HEAVY);
    vline_middle_up(HEAVY);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_light_left(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
}

static void
draw_box_drawings_light_up(uint8_t *buf, int width, int height, int stride, int dpi)
{
    vline_middle_up(LIGHT);
}

static void
draw_box_drawings_light_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(LIGHT);
}

static void
draw_box_drawings_light_down(uint8_t *buf, int width, int height, int stride, int dpi)
{
    vline_middle_down(LIGHT);
}

static void
draw_box_drawings_heavy_left(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
}

static void
draw_box_drawings_heavy_up(uint8_t *buf, int width, int height, int stride, int dpi)
{
    vline_middle_up(HEAVY);
}

static void
draw_box_drawings_heavy_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_right(HEAVY);
}

static void
draw_box_drawings_heavy_down(uint8_t *buf, int width, int height, int stride, int dpi)
{
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_light_left_and_heavy_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(LIGHT);
    hline_middle_right(HEAVY);
}

static void
draw_box_drawings_light_up_and_heavy_down(uint8_t *buf, int width, int height, int stride, int dpi)
{
    vline_middle_up(LIGHT);
    vline_middle_down(HEAVY);
}

static void
draw_box_drawings_heavy_left_and_light_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    hline_middle_left(HEAVY);
    hline_middle_right(LIGHT);
}

static void
draw_box_drawings_heavy_up_and_light_down(uint8_t *buf, int width, int height, int stride, int dpi)
{
    vline_middle_up(HEAVY);
    vline_middle_down(LIGHT);
}

static void
draw_glyph(wchar_t wc, uint8_t *buf, int width, int height, int stride, int dpi)
{
    switch (wc) {
    case 0x2500: draw_box_drawings_light_horizontal(buf, width, height, stride, dpi); break;
    case 0x2501: draw_box_drawings_heavy_horizontal(buf, width, height, stride, dpi); break;
    case 0x2502: draw_box_drawings_light_vertical(buf, width, height, stride, dpi); break;
    case 0x2503: draw_box_drawings_heavy_vertical(buf, width, height, stride, dpi); break;
    case 0x250c: draw_box_drawings_light_down_and_right(buf, width, height, stride, dpi); break;
    case 0x250d: draw_box_drawings_down_light_and_right_heavy(buf, width, height, stride, dpi); break;
    case 0x250e: draw_box_drawings_down_heavy_and_right_light(buf, width, height, stride, dpi); break;
    case 0x250f: draw_box_drawings_heavy_down_and_right(buf, width, height, stride, dpi); break;

    case 0x2504:
    case 0x2505:
    case 0x2506:
    case 0x2507:
    case 0x2508:
    case 0x2509:
    case 0x250a:
    case 0x250b:
        LOG_WARN("unimplemented: box drawing: wc=%04lx", (long)wc);
        break;

    case 0x2510: draw_box_drawings_light_down_and_left(buf, width, height, stride, dpi); break;
    case 0x2511: draw_box_drawings_down_light_and_left_heavy(buf, width, height, stride, dpi); break;
    case 0x2512: draw_box_drawings_down_heavy_and_left_light(buf, width, height, stride, dpi); break;
    case 0x2513: draw_box_drawings_heavy_down_and_left(buf, width, height, stride, dpi); break;
    case 0x2514: draw_box_drawings_light_up_and_right(buf, width, height, stride, dpi); break;
    case 0x2515: draw_box_drawings_up_light_and_right_heavy(buf, width, height, stride, dpi); break;
    case 0x2516: draw_box_drawings_up_heavy_and_right_light(buf, width, height, stride, dpi); break;
    case 0x2517: draw_box_drawings_heavy_up_and_right(buf, width, height, stride, dpi); break;
    case 0x2518: draw_box_drawings_light_up_and_left(buf, width, height, stride, dpi); break;
    case 0x2519: draw_box_drawings_up_light_and_left_heavy(buf, width, height, stride, dpi); break;
    case 0x251a: draw_box_drawings_up_heavy_and_left_light(buf, width, height, stride, dpi); break;
    case 0x251b: draw_box_drawings_heavy_up_and_left(buf, width, height, stride, dpi); break;
    case 0x251c: draw_box_drawings_light_vertical_and_right(buf, width, height, stride, dpi); break;
    case 0x251d: draw_box_drawings_vertical_light_and_right_heavy(buf, width, height, stride, dpi); break;
    case 0x251e: draw_box_drawings_up_heavy_and_right_down_light(buf, width, height, stride, dpi); break;
    case 0x251f: draw_box_drawings_down_heavy_and_right_up_light(buf, width, height, stride, dpi); break;

    case 0x2520: draw_box_drawings_vertical_heavy_and_right_light(buf, width, height, stride, dpi); break;
    case 0x2521: draw_box_drawings_down_light_and_right_up_heavy(buf, width, height, stride, dpi); break;
    case 0x2522: draw_box_drawings_up_light_and_right_down_heavy(buf, width, height, stride, dpi); break;
    case 0x2523: draw_box_drawings_heavy_vertical_and_right(buf, width, height, stride, dpi); break;
    case 0x2524: draw_box_drawings_light_vertical_and_left(buf, width, height, stride, dpi); break;
    case 0x2525: draw_box_drawings_vertical_light_and_left_heavy(buf, width, height, stride, dpi); break;
    case 0x2526: draw_box_drawings_up_heavy_and_left_down_light(buf, width, height, stride, dpi); break;
    case 0x2527: draw_box_drawings_down_heavy_and_left_up_light(buf, width, height, stride, dpi); break;
    case 0x2528: draw_box_drawings_vertical_heavy_and_left_light(buf, width, height, stride, dpi); break;
    case 0x2529: draw_box_drawings_down_light_and_left_up_heavy(buf, width, height, stride, dpi); break;
    case 0x252a: draw_box_drawings_up_light_and_left_down_heavy(buf, width, height, stride, dpi); break;
    case 0x252b: draw_box_drawings_heavy_vertical_and_left(buf, width, height, stride, dpi); break;
    case 0x252c: draw_box_drawings_light_down_and_horizontal(buf, width, height, stride, dpi); break;
    case 0x252d: draw_box_drawings_left_heavy_and_right_down_light(buf, width, height, stride, dpi); break;
    case 0x252e: draw_box_drawings_right_heavy_and_left_down_light(buf, width, height, stride, dpi); break;
    case 0x252f: draw_box_drawings_down_light_and_horizontal_heavy(buf, width, height, stride, dpi); break;

    case 0x2530: draw_box_drawings_down_heavy_and_horizontal_light(buf, width, height, stride, dpi); break;
    case 0x2531: draw_box_drawings_right_light_and_left_down_heavy(buf, width, height, stride, dpi); break;
    case 0x2532: draw_box_drawings_left_light_and_right_down_heavy(buf, width, height, stride, dpi); break;
    case 0x2533: draw_box_drawings_heavy_down_and_horizontal(buf, width, height, stride, dpi); break;
    case 0x2534: draw_box_drawings_light_up_and_horizontal(buf, width, height, stride, dpi); break;
    case 0x2535: draw_box_drawings_left_heavy_and_right_up_light(buf, width, height, stride, dpi); break;
    case 0x2536: draw_box_drawings_right_heavy_and_left_up_light(buf, width, height, stride, dpi); break;
    case 0x2537: draw_box_drawings_up_light_and_horizontal_heavy(buf, width, height, stride, dpi); break;
    case 0x2538: draw_box_drawings_up_heavy_and_horizontal_light(buf, width, height, stride, dpi); break;
    case 0x2539: draw_box_drawings_right_light_and_left_up_heavy(buf, width, height, stride, dpi); break;
    case 0x253a: draw_box_drawings_left_light_and_right_up_heavy(buf, width, height, stride, dpi); break;
    case 0x253b: draw_box_drawings_heavy_up_and_horizontal(buf, width, height, stride, dpi); break;
    case 0x253c: draw_box_drawings_light_vertical_and_horizontal(buf, width, height, stride, dpi); break;
    case 0x253d: draw_box_drawings_left_heavy_and_right_vertical_light(buf, width, height, stride, dpi); break;
    case 0x253e: draw_box_drawings_right_heavy_and_left_vertical_light(buf, width, height, stride, dpi); break;
    case 0x253f: draw_box_drawings_vertical_light_and_horizontal_heavy(buf, width, height, stride, dpi); break;

    case 0x2540: draw_box_drawings_up_heavy_and_down_horizontal_light(buf, width, height, stride, dpi); break;
    case 0x2541: draw_box_drawings_down_heavy_and_up_horizontal_light(buf, width, height, stride, dpi); break;
    case 0x2542: draw_box_drawings_vertical_heavy_and_horizontal_light(buf, width, height, stride, dpi); break;
    case 0x2543: draw_box_drawings_left_up_heavy_and_right_down_light(buf, width, height, stride, dpi); break;
    case 0x2544: draw_box_drawings_right_up_heavy_and_left_down_light(buf, width, height, stride, dpi); break;
    case 0x2545: draw_box_drawings_left_down_heavy_and_right_up_light(buf, width, height, stride, dpi); break;
    case 0x2546: draw_box_drawings_right_down_heavy_and_left_up_light(buf, width, height, stride, dpi); break;
    case 0x2547: draw_box_drawings_down_light_and_up_horizontal_heavy(buf, width, height, stride, dpi); break;
    case 0x2548: draw_box_drawings_up_light_and_down_horizontal_heavy(buf, width, height, stride, dpi); break;
    case 0x2549: draw_box_drawings_right_light_and_left_vertical_heavy(buf, width, height, stride, dpi); break;
    case 0x254a: draw_box_drawings_left_light_and_right_vertical_heavy(buf, width, height, stride, dpi); break;
    case 0x254b: draw_box_drawings_heavy_vertical_and_horizontal(buf, width, height, stride, dpi); break;

    case 0x254c:
    case 0x254d:
    case 0x254e:
    case 0x254f:
        LOG_WARN("unimplemented: box drawing: wc=%04lx", (long)wc);
        break;

    case 0x2550:
    case 0x2551:
    case 0x2552:
    case 0x2553:
    case 0x2554:
    case 0x2555:
    case 0x2556:
    case 0x2557:
    case 0x2558:
    case 0x2559:
    case 0x255a:
    case 0x255b:
    case 0x255c:
    case 0x255d:
    case 0x255e:
    case 0x255f:
        LOG_WARN("unimplemented: box drawing: wc=%04lx", (long)wc);
        break;

    case 0x2560:
    case 0x2561:
    case 0x2562:
    case 0x2563:
    case 0x2564:
    case 0x2565:
    case 0x2566:
    case 0x2567:
    case 0x2568:
    case 0x2569:
    case 0x256a:
    case 0x256b:
    case 0x256c:
    case 0x256d:
    case 0x256e:
    case 0x256f:
        LOG_WARN("unimplemented: box drawing: wc=%04lx", (long)wc);
        break;

    case 0x2574: draw_box_drawings_light_left(buf, width, height, stride, dpi); break;
    case 0x2575: draw_box_drawings_light_up(buf, width, height, stride, dpi); break;
    case 0x2576: draw_box_drawings_light_right(buf, width, height, stride, dpi); break;
    case 0x2577: draw_box_drawings_light_down(buf, width, height, stride, dpi); break;
    case 0x2578: draw_box_drawings_heavy_left(buf, width, height, stride, dpi); break;
    case 0x2579: draw_box_drawings_heavy_up(buf, width, height, stride, dpi); break;
    case 0x257a: draw_box_drawings_heavy_right(buf, width, height, stride, dpi); break;
    case 0x257b: draw_box_drawings_heavy_down(buf, width, height, stride, dpi); break;
    case 0x257c: draw_box_drawings_light_left_and_heavy_right(buf, width, height, stride, dpi); break;
    case 0x257d: draw_box_drawings_light_up_and_heavy_down(buf, width, height, stride, dpi); break;
    case 0x257e: draw_box_drawings_heavy_left_and_light_right(buf, width, height, stride, dpi); break;
    case 0x257f: draw_box_drawings_heavy_up_and_light_down(buf, width, height, stride, dpi); break;

    case 0x2570:
    case 0x2571:
    case 0x2572:
    case 0x2573:
        LOG_WARN("unimplemented: box drawing: wc=%04lx", (long)wc);
        break;

    case 0x2580:
    case 0x2581:
    case 0x2582:
    case 0x2583:
    case 0x2584:
    case 0x2585:
    case 0x2586:
    case 0x2587:
    case 0x2588:
    case 0x2589:
    case 0x258a:
    case 0x258b:
    case 0x258c:
    case 0x258d:
    case 0x258e:
    case 0x258f:
        LOG_WARN("unimplemented: box drawing: wc=%04lx", (long)wc);
        break;

    case 0x2590:
    case 0x2591:
    case 0x2592:
    case 0x2593:
    case 0x2594:
    case 0x2595:
    case 0x2596:
    case 0x2597:
    case 0x2598:
    case 0x2599:
    case 0x259a:
    case 0x259b:
    case 0x259c:
    case 0x259d:
    case 0x259e:
    case 0x259f:
        LOG_WARN("unimplemented: box drawing: wc=%04lx", (long)wc);
        break;
    }
}

struct fcft_glyph *
box_drawing(struct terminal *term, wchar_t wc)
{
    int width = term->cell_width;
    int height = term->cell_height;
    int stride = stride_for_format_and_width(PIXMAN_a1, width);
    uint8_t *data = xcalloc(height * stride, 1);

    pixman_image_t *pix = pixman_image_create_bits_no_clear(
        PIXMAN_a1, width, height, (uint32_t*)data, stride);

    if (pix == NULL) {
        errno = ENOMEM;
        perror(__func__);
        abort();
    }

    draw_glyph(wc, data, width, height, stride, term->font_dpi);

    struct fcft_glyph *glyph = xmalloc(sizeof(*glyph));
    *glyph = (struct fcft_glyph){
        .wc = wc,
        .cols = 1,
        .pix = pix,
        .x = 0,
        .y = term->fonts[0]->ascent,
        .width = width,
        .height = height,
        .advance = {
            .x = width,
            .y = height,
        },
    };
    return glyph;
}
