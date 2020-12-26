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
draw_glyph(wchar_t wc, pixman_image_t *pix, int width, int height, int stride)
{
    switch (wc) {
    case 0x2500:
    case 0x2501:
    case 0x2502:
    case 0x2503:
    case 0x2504:
    case 0x2505:
    case 0x2506:
    case 0x2507:
    case 0x2508:
    case 0x2509:
    case 0x250a:
    case 0x250b:
    case 0x250c:
    case 0x250d:
    case 0x250e:
    case 0x250f:

    case 0x2510:
    case 0x2511:
    case 0x2512:
    case 0x2513:
    case 0x2514:
    case 0x2515:
    case 0x2516:
    case 0x2517:
    case 0x2518:
    case 0x2519:
    case 0x251a:
    case 0x251b:
    case 0x251c:
    case 0x251d:
    case 0x251e:
    case 0x251f:

    case 0x2520:
    case 0x2521:
    case 0x2522:
    case 0x2523:
    case 0x2524:
    case 0x2525:
    case 0x2526:
    case 0x2527:
    case 0x2528:
    case 0x2529:
    case 0x252a:
    case 0x252b:
    case 0x252c:
    case 0x252d:
    case 0x252e:
    case 0x252f:

    case 0x2530:
    case 0x2531:
    case 0x2532:
    case 0x2533:
    case 0x2534:
    case 0x2535:
    case 0x2536:
    case 0x2537:
    case 0x2538:
    case 0x2539:
    case 0x253a:
    case 0x253b:
    case 0x253c:
    case 0x253d:
    case 0x253e:
    case 0x253f:

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
        //assert(false);
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

    draw_glyph(wc, pix, width, height, stride);

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
