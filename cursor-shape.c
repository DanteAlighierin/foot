#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "cursor-shape"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "cursor-shape.h"
#include "debug.h"
#include "util.h"

const char *const *
cursor_shape_to_string(enum cursor_shape shape)
{
    static const char *const table[][CURSOR_SHAPE_COUNT]= {
        [CURSOR_SHAPE_NONE] = {NULL},
        [CURSOR_SHAPE_HIDDEN] = {"hidden", NULL},
        [CURSOR_SHAPE_LEFT_PTR] = {"default", "left_ptr", NULL},
        [CURSOR_SHAPE_TEXT] = {"text", "xterm", NULL},
        [CURSOR_SHAPE_TOP_LEFT_CORNER] = {"nw-resize", "top_left_corner", NULL},
        [CURSOR_SHAPE_TOP_RIGHT_CORNER] = {"ne-resize", "top_right_corner", NULL},
        [CURSOR_SHAPE_BOTTOM_LEFT_CORNER] = {"sw-resize", "bottom_left_corner", NULL},
        [CURSOR_SHAPE_BOTTOM_RIGHT_CORNER] = {"se-resize", "bottom_right_corner", NULL},
        [CURSOR_SHAPE_LEFT_SIDE] = {"w-resize", "left_side", NULL},
        [CURSOR_SHAPE_RIGHT_SIDE] = {"e-resize", "right_side", NULL},
        [CURSOR_SHAPE_TOP_SIDE] = {"n-resize", "top_side", NULL},
        [CURSOR_SHAPE_BOTTOM_SIDE] = {"s-resize", "bottom_side", NULL},

    };

    xassert(shape <= ALEN(table));
    return table[shape];
}

enum wp_cursor_shape_device_v1_shape
cursor_shape_to_server_shape(enum cursor_shape shape)
{
    static const enum wp_cursor_shape_device_v1_shape table[CURSOR_SHAPE_COUNT] = {
        [CURSOR_SHAPE_LEFT_PTR] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
        [CURSOR_SHAPE_TEXT] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT,
        [CURSOR_SHAPE_TOP_LEFT_CORNER] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE,
        [CURSOR_SHAPE_TOP_RIGHT_CORNER] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE,
        [CURSOR_SHAPE_BOTTOM_LEFT_CORNER] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE,
        [CURSOR_SHAPE_BOTTOM_RIGHT_CORNER] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE,
        [CURSOR_SHAPE_LEFT_SIDE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE,
        [CURSOR_SHAPE_RIGHT_SIDE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE,
        [CURSOR_SHAPE_TOP_SIDE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE,
        [CURSOR_SHAPE_BOTTOM_SIDE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE,
    };

    xassert(shape <= ALEN(table));
    xassert(table[shape] != 0);
    return table[shape];
}

enum wp_cursor_shape_device_v1_shape
cursor_string_to_server_shape(const char *xcursor)
{
    if (xcursor == NULL)
        return 0;

    static const char *const table[][2] = {
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT] = {"default", "left_ptr"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU] = {"context-menu"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP] = {"help", "question_arrow"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER] = {"pointer", "hand"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS] = {"progress", "left_ptr_watch"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT] = {"wait", "watch"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL] = {"cell"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR] = {"crosshair", "cross"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT] = {"text", "xterm"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT] = {"vertical-text"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS] = {"alias", "dnd-link"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY] = {"copy", "dnd-copy"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE] = {"move"},  /* dnd-move? */
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP] = {"no-drop", "dnd-no-drop"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED] = {"not-allowed", "crossed_circle"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB] = {"grab", "hand1"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING] = {"grabbing"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE] = {"e-resize", "right_side"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE] = {"n-resize", "top_side"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE] = {"ne-resize", "top_right_corner"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE] = {"nw-resize", "top_left_corner"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE] = {"s-resize", "bottom_side"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE] = {"se-resize", "bottom_right_corner"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE] = {"sw-resize", "bottom_left_corner"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE] = {"w-resize", "left_side"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE] = {"ew-resize", "sb_h_double_arrow"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE] = {"ns-resize", "sb_v_double_arrow"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE] = {"nesw-resize", "fd_double_arrow"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE] = {"nwse-resize", "bd_double_arrow"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE] = {"col-resize", "sb_h_double_arrow"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE] = {"row-resize", "sb_v_double_arrow"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL] = {"all-scroll", "fleur"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN] = {"zoom-in"},
        [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT] = {"zoom-out"},
    };

    for (size_t i = 0; i < ALEN(table); i++) {
        for (size_t j = 0; j < ALEN(table[i]); j++) {
            if (table[i][j] != NULL && streq(xcursor, table[i][j])) {
                return i;
            }
        }
    }

    return 0;
}
