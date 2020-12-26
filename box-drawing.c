#include "box-drawing.h"

#include <stdio.h>
#include <errno.h>

#define LOG_MODULE "box-drawing"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "stride.h"
#include "terminal.h"
#include "xmalloc.h"

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

static void
draw_box_drawings_light_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    const int thick = 1 * dpi / 72;
    hline(buf, 0, width, (height - thick) / 2, thick, stride);
}

static void
draw_box_drawings_heavy_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    assert(false);
}

static void
draw_box_drawings_light_vertical(uint8_t *buf, int width, int height, int stride, int dpi)
{
    int thick = 1 * dpi / 72;
    vline(buf, 0, height, (width - thick) / 2, thick, stride);
}

static void
draw_box_drawings_light_down_and_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    int thick = 1 * dpi / 72;
    int hmid = (height - thick) / 2;
    int vmid = (width - thick) / 2;
    hline(buf, vmid, width, hmid, thick, stride);
    vline(buf, hmid, height, vmid, thick, stride);
}

static void
draw_box_drawings_light_down_and_left(uint8_t *buf, int width, int height, int stride, int dpi)
{
    int thick = 1 * dpi / 72;
    int hmid = (height - thick) / 2;
    int vmid = (width - thick) / 2;
    hline(buf, 0, vmid, hmid, thick, stride);
    vline(buf, hmid, height, vmid, thick, stride);
}

static void
draw_box_drawings_light_up_and_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    int thick = 1 * dpi / 72;
    int hmid = (height - thick) / 2;
    int vmid = (width - thick) / 2;
    hline(buf, vmid, width, hmid, thick, stride);
    vline(buf, 0, hmid, vmid, thick, stride);
}

static void
draw_box_drawings_light_up_and_left(uint8_t *buf, int width, int height, int stride, int dpi)
{
    int thick = 1 * dpi / 72;
    int hmid = (height - thick) / 2;
    int vmid = (width - thick) / 2;
    hline(buf, 0, vmid, hmid, thick, stride);
    vline(buf, 0, hmid + 1, vmid, thick, stride);
}

static void
draw_box_drawings_light_vertical_and_right(uint8_t *buf, int width, int height, int stride, int dpi)
{
    int thick = 1 * dpi / 72;
    int hmid = (height - thick) / 2;
    int vmid = (width - thick) / 2;
    hline(buf, vmid, width, hmid, thick, stride);
    vline(buf, 0, height, vmid, thick, stride);
}

static void
draw_box_drawings_light_vertical_and_left(uint8_t *buf, int width, int height, int stride, int dpi)
{
    int thick = 1 * dpi / 72;
    int hmid = (height - thick) / 2;
    int vmid = (width - thick) / 2;
    hline(buf, 0, vmid, hmid, thick, stride);
    vline(buf, 0, height, vmid, thick, stride);
}

static void
draw_box_drawings_light_down_and_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    int thick = 1 * dpi / 72;
    int hmid = (height - thick) / 2;
    int vmid = (width - thick) / 2;
    hline(buf, 0, width, hmid, thick, stride);
    vline(buf, hmid, height, vmid, thick, stride);
}

static void
draw_box_drawings_light_up_and_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    int thick = 1 * dpi / 72;
    int hmid = (height - thick) / 2;
    int vmid = (width - thick) / 2;
    hline(buf, 0, width, hmid, thick, stride);
    vline(buf, 0, hmid, vmid, thick, stride);
}

static void
draw_box_drawings_light_vertical_and_horizontal(uint8_t *buf, int width, int height, int stride, int dpi)
{
    draw_box_drawings_light_vertical(buf, width, height, stride, dpi);
    draw_box_drawings_light_horizontal(buf, width, height, stride, dpi);
}

static void
draw_glyph(wchar_t wc, uint8_t *buf, int width, int height, int stride, int dpi)
{
    switch (wc) {
    case 0x2500: draw_box_drawings_light_horizontal(buf, width, height, stride, dpi); break;
    case 0x2501: draw_box_drawings_heavy_horizontal(buf, width, height, stride, dpi); break;
    case 0x2502: draw_box_drawings_light_vertical(buf, width, height, stride, dpi); break;
    case 0x250c: draw_box_drawings_light_down_and_right(buf, width, height, stride, dpi); break;

    case 0x2503:
    case 0x2504:
    case 0x2505:
    case 0x2506:
    case 0x2507:
    case 0x2508:
    case 0x2509:
    case 0x250a:
    case 0x250b:
    case 0x250d:
    case 0x250e:
    case 0x250f:
        LOG_WARN("unimplemented: box drawing: wc=%04lx", (long)wc);
        break;

    case 0x2510: draw_box_drawings_light_down_and_left(buf, width, height, stride, dpi); break;
    case 0x2514: draw_box_drawings_light_up_and_right(buf, width, height, stride, dpi); break;
    case 0x2518: draw_box_drawings_light_up_and_left(buf, width, height, stride, dpi); break;
    case 0x251c: draw_box_drawings_light_vertical_and_right(buf, width, height, stride, dpi); break;

    case 0x2511:
    case 0x2512:
    case 0x2513:
    case 0x2515:
    case 0x2516:
    case 0x2517:
    case 0x2519:
    case 0x251a:
    case 0x251b:
    case 0x251d:
    case 0x251e:
    case 0x251f:
        LOG_WARN("unimplemented: box drawing: wc=%04lx", (long)wc);
        break;

    case 0x2524: draw_box_drawings_light_vertical_and_left(buf, width, height, stride, dpi); break;
    case 0x252c: draw_box_drawings_light_down_and_horizontal(buf, width, height, stride, dpi); break;

    case 0x2520:
    case 0x2521:
    case 0x2522:
    case 0x2523:
    case 0x2525:
    case 0x2526:
    case 0x2527:
    case 0x2528:
    case 0x2529:
    case 0x252a:
    case 0x252b:
    case 0x252d:
    case 0x252e:
    case 0x252f:
        LOG_WARN("unimplemented: box drawing: wc=%04lx", (long)wc);
        break;

    case 0x2534: draw_box_drawings_light_up_and_horizontal(buf, width, height, stride, dpi); break;
    case 0x253c: draw_box_drawings_light_vertical_and_horizontal(buf, width, height, stride, dpi); break;

    case 0x2530:
    case 0x2531:
    case 0x2532:
    case 0x2533:
    case 0x2535:
    case 0x2536:
    case 0x2537:
    case 0x2538:
    case 0x2539:
    case 0x253a:
    case 0x253b:
    case 0x253d:
    case 0x253e:
    case 0x253f:
        LOG_WARN("unimplemented: box drawing: wc=%04lx", (long)wc);
        break;

    case 0x2540:
    case 0x2541:
    case 0x2542:
    case 0x2543:
    case 0x2544:
    case 0x2545:
    case 0x2546:
    case 0x2547:
    case 0x2548:
    case 0x2549:
    case 0x254a:
    case 0x254b:
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

    case 0x2570:
    case 0x2571:
    case 0x2572:
    case 0x2573:
    case 0x2574:
    case 0x2575:
    case 0x2576:
    case 0x2577:
    case 0x2578:
    case 0x2579:
    case 0x257a:
    case 0x257b:
    case 0x257c:
    case 0x257d:
    case 0x257e:
    case 0x257f:
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
    LOG_DBG("rendering 0x%04lx", (long)wc);

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
