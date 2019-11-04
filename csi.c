#include "csi.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#if defined(_DEBUG)
 #include <stdio.h>
#endif

#define LOG_MODULE "csi"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "grid.h"
#include "vt.h"
#include "selection.h"

#define min(x, y) ((x) < (y) ? (x) : (y))

#define UNHANDLED()     LOG_ERR("unhandled: %s", csi_as_string(term, final))
#define UNHANDLED_SGR() LOG_ERR("unhandled: %s", csi_as_string(term, 'm'))

static void
sgr_reset(struct terminal *term)
{
    memset(&term->vt.attrs, 0, sizeof(term->vt.attrs));
    term->vt.attrs.fg = term->colors.fg;
    term->vt.attrs.bg = term->colors.bg;
}

static const char *
csi_as_string(struct terminal *term, uint8_t final)
{
    static char msg[1024];
    int c = snprintf(msg, sizeof(msg), "CSI: ");

    for (size_t i = 0; i < sizeof(term->vt.private) / sizeof(term->vt.private[0]); i++) {
        if (term->vt.private[i] == 0)
            break;
        c += snprintf(&msg[c], sizeof(msg) - c, "%c", term->vt.private[i]);
    }

    for (size_t i = 0; i < term->vt.params.idx; i++){
        c += snprintf(&msg[c], sizeof(msg) - c, "%d",
                      term->vt.params.v[i].value);

        for (size_t j = 0; j < term->vt.params.v[i].sub.idx; j++) {
            c += snprintf(&msg[c], sizeof(msg) - c, ":%d",
                          term->vt.params.v[i].sub.value[j]);
        }

        c += snprintf(&msg[c], sizeof(msg) - c, "%s",
                      i == term->vt.params.idx - 1 ? "" : ";");
    }

    c += snprintf(&msg[c], sizeof(msg) - c, "%c (%zu parameters)",
                  final, term->vt.params.idx);
    return msg;
}

static void
csi_sgr(struct terminal *term)
{
    if (term->vt.params.idx == 0) {
        sgr_reset(term);
        return;
    }

    for (size_t i = 0; i < term->vt.params.idx; i++) {
        const int param = term->vt.params.v[i].value;

        switch (param) {
        case 0:
            sgr_reset(term);
            break;

        case 1: term->vt.attrs.bold = true; break;
        case 2: term->vt.attrs.dim = true; break;
        case 3: term->vt.attrs.italic = true; break;
        case 4: term->vt.attrs.underline = true; break;
        case 5: term->vt.attrs.blink = true; break;
        case 6: LOG_WARN("ignored: rapid blink"); break;
        case 7: term->vt.attrs.reverse = true; break;
        case 8: term->vt.attrs.conceal = true; break;
        case 9: term->vt.attrs.strikethrough = true; break;

        case 21: term->vt.attrs.bold = false; break;
        case 22: term->vt.attrs.bold = term->vt.attrs.dim = false; break;
        case 23: term->vt.attrs.italic = false; break;
        case 24: term->vt.attrs.underline = false; break;
        case 25: term->vt.attrs.blink = false; break;
        case 26: break;  /* rapid blink, ignored */
        case 27: term->vt.attrs.reverse = false; break;
        case 28: term->vt.attrs.conceal = false; break;
        case 29: term->vt.attrs.strikethrough = false; break;

        /* Regular foreground colors */
        case 30:
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
            term->vt.attrs.have_fg = 1;
            term->vt.attrs.fg = term->colors.table[param - 30];
            break;

        case 38: {
            /* Indexed: 38;5;<idx> */
            if (term->vt.params.idx - i - 1 >= 2 &&
                term->vt.params.v[i + 1].value == 5)
            {
                uint8_t idx = term->vt.params.v[i + 2].value;
                term->vt.attrs.have_fg = 1;
                term->vt.attrs.fg = term->colors.table[idx];
                i += 2;

            }

            /* RGB: 38;2;<r>;<g>;<b> */
            else if (term->vt.params.idx - i - 1 >= 4 &&
                     term->vt.params.v[i + 1].value == 2)
            {
                uint8_t r = term->vt.params.v[i + 2].value;
                uint8_t g = term->vt.params.v[i + 3].value;
                uint8_t b = term->vt.params.v[i + 4].value;
                term->vt.attrs.have_fg = 1;
                term->vt.attrs.fg = r << 16 | g << 8 | b;
                i += 4;
            }

            /* Sub-parameter style: 38:2:... */
            else if (term->vt.params.v[i].sub.idx >= 2 &&
                     term->vt.params.v[i].sub.value[0] == 2)
            {
                const struct vt_param *param = &term->vt.params.v[i];
                const int color_space_id = param->sub.value[1];

                switch (color_space_id) {
                case 0:   /* Implementation defined - we map it to '2' */
                case 2: { /* RGB - 38:2:2:<r>:<g>:<b> */
                    if (param->sub.idx < 5) {
                        UNHANDLED_SGR();
                        break;
                    }

                    uint8_t r = param->sub.value[2];
                    uint8_t g = param->sub.value[3];
                    uint8_t b = param->sub.value[4];
                    /* 5 - unused */
                    /* 6 - CS tolerance */
                    /* 7 - color space associated with tolerance */

                    term->vt.attrs.have_fg = 1;
                    term->vt.attrs.fg = r << 16 | g << 8 | b;
                    break;
                }

                case 5: { /* Indexed - 38:2:5:<idx> */
                    if (param->sub.idx < 3) {
                        UNHANDLED_SGR();
                        break;
                    }

                    uint8_t idx = param->sub.value[2];
                    term->vt.attrs.have_fg = 1;
                    term->vt.attrs.fg = term->colors.table[idx];
                    break;
                }

                case 1: /* Transparent */
                case 3: /* CMY */
                case 4: /* CMYK */
                    UNHANDLED_SGR();
                    break;
                }
            }

            /* Unrecognized */
            else
                UNHANDLED_SGR();

            break;
        }

        case 39:
            term->vt.attrs.have_fg = 0;
            break;

        /* Regular background colors */
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
            term->vt.attrs.have_bg = 1;
            term->vt.attrs.bg = term->colors.table[param - 40];
            break;

        case 48: {
            /* Indexed: 48;5;<idx> */
            if (term->vt.params.idx - i - 1 >= 2 &&
                term->vt.params.v[i + 1].value == 5)
            {
                uint8_t idx = term->vt.params.v[i + 2].value;
                term->vt.attrs.have_bg = 1;
                term->vt.attrs.bg = term->colors.table[idx];
                i += 2;

            }

            /* RGB: 48;2;<r>;<g>;<b> */
            else if (term->vt.params.idx - i - 1 >= 4 &&
                     term->vt.params.v[i + 1].value == 2)
            {
                uint8_t r = term->vt.params.v[i + 2].value;
                uint8_t g = term->vt.params.v[i + 3].value;
                uint8_t b = term->vt.params.v[i + 4].value;
                term->vt.attrs.have_bg = 1;
                term->vt.attrs.bg = r << 16 | g << 8 | b;
                i += 4;
            }

            /* Sub-parameter style: 48:2:... */
            else if (term->vt.params.v[i].sub.idx >= 2 &&
                     term->vt.params.v[i].sub.value[0] == 2)
            {
                const struct vt_param *param = &term->vt.params.v[i];
                const int color_space_id = param->sub.value[1];

                switch (color_space_id) {
                case 0:   /* Implementation defined - we map it to '2' */
                case 2: { /* RGB - 48:2:2:<r>:<g>:<b> */
                    if (param->sub.idx < 5) {
                        UNHANDLED_SGR();
                        break;
                    }

                    uint8_t r = param->sub.value[2];
                    uint8_t g = param->sub.value[3];
                    uint8_t b = param->sub.value[4];
                    /* 5 - unused */
                    /* 6 - CS tolerance */
                    /* 7 - color space associated with tolerance */

                    term->vt.attrs.have_bg = 1;
                    term->vt.attrs.bg = r << 16 | g << 8 | b;
                    break;
                }

                case 5: { /* Indexed - 48:2:5:<idx> */
                    if (param->sub.idx < 3) {
                        UNHANDLED_SGR();
                        break;
                    }

                    uint8_t idx = param->sub.value[2];
                    term->vt.attrs.have_bg = 1;
                    term->vt.attrs.bg = term->colors.table[idx];
                    break;
                }

                case 1: /* Transparent */
                case 3: /* CMY */
                case 4: /* CMYK */
                    UNHANDLED_SGR();
                    break;
                }
            }

            else
                UNHANDLED_SGR();

            break;
        }
        case 49:
            term->vt.attrs.have_bg = 0;
            break;

        /* Bright foreground colors */
        case 90:
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
            term->vt.attrs.have_fg = 1;
            term->vt.attrs.fg = term->colors.table[param - 90 + 8];
            break;

        /* Bright background colors */
        case 100:
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            term->vt.attrs.have_bg = 1;
            term->vt.attrs.bg = term->colors.table[param - 100 + 8];
            break;

        default:
            UNHANDLED_SGR();
            break;
        }
    }
}

void
csi_dispatch(struct terminal *term, uint8_t final)
{
    LOG_DBG("%s", csi_as_string(term, final));

    switch (term->vt.private[0]) {
    case 0: {
        switch (final) {
        case 'c':
            term_to_slave(term, "\033[?6c", 5);
            break;

        case 'd': {
            /* VPA - vertical line position absolute */
            int row = min(vt_param_get(term, 0, 1), term->rows);
            term_cursor_to(term, row - 1, term->cursor.col);
            break;
        }

        case 'm':
            csi_sgr(term);
            break;

        case 'A':
            term_cursor_up(term, vt_param_get(term, 0, 1));
            break;

        case 'e':
        case 'B':
            term_cursor_down(term, vt_param_get(term, 0, 1));
            break;

        case 'C':
            term_cursor_right(term, vt_param_get(term, 0, 1));
            break;

        case 'D':
            term_cursor_left(term, vt_param_get(term, 0, 1));
            break;

        case 'G': {
            /* Cursor horizontal absolute */
            int col = min(vt_param_get(term, 0, 1), term->cols);
            term_cursor_to(term, term->cursor.row, col - 1);
            break;
        }

        case 'H': {
            /* Move cursor */
            int row = min(vt_param_get(term, 0, 1), term->rows);
            int col = min(vt_param_get(term, 1, 1), term->cols);
            term_cursor_to(term, row - 1, col - 1);
            break;
        }

        case 'J': {
            /* Erase screen */

            int param = vt_param_get(term, 0, 0);
            switch (param) {
            case 0:
                /* From cursor to end of screen */
                term_erase(
                    term,
                    &term->cursor,
                    &(struct coord){term->cols - 1, term->rows - 1});
                break;

            case 1:
                /* From start of screen to cursor */
                term_erase(term, &(struct coord){0, 0}, &term->cursor);
                break;

            case 2:
                /* Erase entire screen */
                term_erase(
                    term,
                    &(struct coord){0, 0},
                    &(struct coord){term->cols - 1, term->rows - 1});
                break;

            case 3: {
                /* Erase scrollback */
                int end = (term->grid->offset + term->rows - 1) % term->grid->num_rows;
                for (size_t i = 0; i < term->grid->num_rows; i++) {
                    if (end >= term->grid->offset) {
                        /* Not wrapped */
                        if (i >= term->grid->offset && i <= end)
                            continue;
                    } else {
                        /* Wrapped */
                        if (i >= term->grid->offset || i <= end)
                            continue;
                    }

                    grid_row_free(term->grid->rows[i]);
                    term->grid->rows[i] = NULL;
                }
                term->grid->view = term->grid->offset;
                term_damage_view(term);
                break;
            }

            default:
                UNHANDLED();
                break;
            }
            break;
        }

        case 'K': {
            /* Erase line */

            int param = vt_param_get(term, 0, 0);
            switch (param) {
            case 0:
                /* From cursor to end of line */
                term_erase(
                    term,
                    &term->cursor,
                    &(struct coord){term->cols - 1, term->cursor.row});
                break;

            case 1:
                /* From start of line to cursor */
                term_erase(
                    term, &(struct coord){0, term->cursor.row}, &term->cursor);
                break;

            case 2:
                /* Entire line */
                term_erase(
                    term,
                    &(struct coord){0, term->cursor.row},
                    &(struct coord){term->cols - 1, term->cursor.row});
                break;

            default:
                UNHANDLED();
                break;
            }

            break;
        }

        case 'L': {
            if (term->cursor.row < term->scroll_region.start ||
                term->cursor.row >= term->scroll_region.end)
                break;

            int count = min(
                vt_param_get(term, 0, 1),
                term->scroll_region.end - term->cursor.row);

            term_scroll_reverse_partial(
                term,
                (struct scroll_region){
                    .start = term->cursor.row,
                    .end = term->scroll_region.end},
                count);
            break;
        }

        case 'M': {
            if (term->cursor.row < term->scroll_region.start ||
                term->cursor.row >= term->scroll_region.end)
                break;

            int count = min(
                vt_param_get(term, 0, 1),
                term->scroll_region.end - term->cursor.row);

            term_scroll_partial(
                term,
                (struct scroll_region){
                    .start = term->cursor.row,
                    .end = term->scroll_region.end},
                count);
            break;
        }

        case 'P': {
            /* DCH: Delete character(s) */

            /* Number of characters to delete */
            int count = min(
                vt_param_get(term, 0, 1), term->cols - term->cursor.col);

            /* Number of characters left after deletion (on current line) */
            int remaining = term->cols - (term->cursor.col + count);

            /* 'Delete' characters by moving the remaining ones */
            memmove(&term->grid->cur_row->cells[term->cursor.col],
                    &term->grid->cur_row->cells[term->cursor.col + count],
                    remaining * sizeof(term->grid->cur_row->cells[0]));

            for (size_t c = 0; c < remaining; c++)
                term->grid->cur_row->cells[term->cursor.col + c].attrs.clean = 0;
            term->grid->cur_row->dirty = true;

            /* Erase the remainder of the line */
            term_erase(
                term,
                &(struct coord){term->cursor.col + remaining, term->cursor.row},
                &(struct coord){term->cols - 1, term->cursor.row});
            break;
        }

        case '@': {
            /* ICH: insert character(s) */

            /* Number of characters to insert */
            int count = min(
                vt_param_get(term, 0, 1), term->cols - term->cursor.col);

            /* Characters to move */
            int remaining = term->cols - (term->cursor.col + count);

            /* Push existing characters */
            memmove(&term->grid->cur_row->cells[term->cursor.col + count],
                    &term->grid->cur_row->cells[term->cursor.col],
                    remaining * sizeof(term->grid->cur_row->cells[0]));
            for (size_t c = 0; c < remaining; c++)
                term->grid->cur_row->cells[term->cursor.col + count + c].attrs.clean = 0;
            term->grid->cur_row->dirty = true;

            /* Erase (insert space characters) */
            term_erase(
                term,
                &term->cursor,
                &(struct coord){term->cursor.col + count - 1, term->cursor.row});
            break;
        }

        case 'S':
            term_scroll(term, vt_param_get(term, 0, 1));
            break;

        case 'T':
            term_scroll_reverse(term, vt_param_get(term, 0, 1));
            break;

        case 'X': {
            /* Erase chars */
            int count = min(
                vt_param_get(term, 0, 1), term->cols - term->cursor.col);

            term_erase(
                term,
                &term->cursor,
                &(struct coord){term->cursor.col + count - 1, term->cursor.row});
            break;
        }

        case 'Z': {
            /* Back tab */
            int col = term->cursor.col;
            col = (col - 8 + 7) / 8 * 8;
            term_cursor_right(term, col - term->cursor.col);
            break;
        }

        case 'h':
            /* Set mode */
            switch (vt_param_get(term, 0, 0)) {
            case 2:   /* Keyboard Action Mode - AM */
                LOG_WARN("unimplemented: keyboard action mode (AM)");
                break;

            case 4:   /* Insert Mode - IRM */
                term->insert_mode = true;
                break;

            case 12:  /* Send/receive Mode - SRM */
                LOG_WARN("unimplemented: send/receive mode (SRM)");
                break;

            case 20:  /* Automatic Newline Mode - LNM */
                LOG_WARN("unimplemented: automatic newline mode (LNM)");
                break;
            }
            break;

        case 'l':
            /* Reset mode */
            switch (vt_param_get(term, 0, 0)) {
            case 4:   /* Insert Mode - IRM */
                term->insert_mode = false;
                break;

            case 2:   /* Keyboard Action Mode - AM */
            case 12:  /* Send/receive Mode - SRM */
            case 20:  /* Automatic Newline Mode - LNM */
                break;
            }
            break;

        case 'r': {
            int start = vt_param_get(term, 0, 1);
            int end = min(vt_param_get(term, 1, term->rows), term->rows);

            if (end > start) {

                /* 1-based */
                term->scroll_region.start = start - 1;
                term->scroll_region.end = end;

                term_cursor_to(term, start - 1, 0);

                LOG_DBG("scroll region: %d-%d",
                        term->scroll_region.start,
                        term->scroll_region.end);
            }
            break;
        }

        case 's':
            term->saved_cursor = term->cursor;
            break;

        case 'u':
            term_restore_cursor(term);
            break;

        case 't': {
            unsigned param = vt_param_get(term, 0, 0);

            switch (param) {
            case 22: { /* push window title */
                /* 0 - icon + title, 1 - icon, 2 - title */
                unsigned what = vt_param_get(term, 1, 0);
                if (what == 0 || what == 2) {
                    tll_push_back(
                        term->window_title_stack, strdup(term->window_title));
                }
                break;
            }

            case 23: { /* pop window title */
                /* 0 - icon + title, 1 - icon, 2 - title */
                unsigned what = vt_param_get(term, 1, 0);
                if (what == 0 || what == 2) {
                    if (tll_length(term->window_title_stack) > 0) {
                        char *title = tll_pop_back(term->window_title_stack);
                        term_set_window_title(term, title);
                        free(title);
                    }
                }
                break;
            }

            case 1001: {
            }

            default:
                LOG_WARN("ignoring %s", csi_as_string(term, final));
                break;
            }
            break;
        }

        case 'n': {
            if (term->vt.params.idx > 0) {
                int param = vt_param_get(term, 0, 0);
                switch (param) {
                case 6: {
                    /* u7 - cursor position query */
                    /* TODO: we use 0-based position, while the xterm
                     * terminfo says the receiver of the reply should
                     * decrement, hence we must add 1 */
                    char reply[64];
                    snprintf(reply, sizeof(reply), "\x1b[%d;%dR",
                             term->cursor.row + 1,
                             term->cursor.col + 1);
                    term_to_slave(term, reply, strlen(reply));
                    break;
                }

                default:
                    UNHANDLED();
                    break;
                }
            } else
                UNHANDLED();

            break;
        }

        default:
            UNHANDLED();
            break;
        }

        break;  /* private == 0 */
    }

    case '?': {
        switch (final) {
        case 'h': {
            for (size_t i = 0; i < term->vt.params.idx; i++) {
                switch (term->vt.params.v[i].value) {
                case 1:
                    term->cursor_keys_mode = CURSOR_KEYS_APPLICATION;
                    break;

                case 5:
                    term->reverse = true;
                    term_damage_all(term);
                    break;

                case 7:
                    term->auto_margin = true;
                    break;

                case 12:
                    /* Ignored */
                    break;

                case 25:
                    term->hide_cursor = false;
                    break;

                case 1000:
                    term->mouse_tracking = MOUSE_CLICK;
                    break;

                case 1002:
                    term->mouse_tracking = MOUSE_DRAG;
                    break;

                case 1003:
                    term->mouse_tracking = MOUSE_MOTION;
                    break;

                case 1004:
                    term->focus_events = true;
                    break;

                case 1005:
                    LOG_WARN("unimplemented: UTF-8 mouse");
                    /* term->mouse_reporting = MOUSE_UTF8; */
                    break;

                case 1006:
                    term->mouse_reporting = MOUSE_SGR;
                    break;

                case 1007:
                    term->alt_scrolling = true;
                    break;

                case 1015:
                    term->mouse_reporting = MOUSE_URXVT;
                    break;

                case 1036:
                    /* metaSendsEscape - we always send escape */
                    break;

                case 1042:
                    LOG_WARN("unimplemented: 'urgency' window manager hint on ctrl-g");
                    break;

                case 1043:
                    LOG_WARN("unimplemented: raise window on ctrl-g");
                    break;

                case 1049:
                    if (term->grid != &term->alt) {
                        selection_cancel(term);

                        term->grid = &term->alt;
                        term->saved_cursor = term->cursor;

                        term_cursor_to(term, term->cursor.row, term->cursor.col);

                        tll_free(term->alt.damage);
                        tll_free(term->alt.scroll_damage);

                        term_erase(
                            term,
                            &(struct coord){0, 0},
                            &(struct coord){term->cols - 1, term->rows - 1});
                    }
                    break;

                case 2004:
                    term->bracketed_paste = true;
                    break;

                default:
                    UNHANDLED();
                    break;
                }
            }
            break;
        }

        case 'l': {
            for (size_t i = 0; i < term->vt.params.idx; i++) {
                switch (term->vt.params.v[i].value) {
                case 1:
                    term->cursor_keys_mode = CURSOR_KEYS_NORMAL;
                    break;

                case 3:
                    LOG_WARN("unimplemented: 132 column mode (DECCOLM, %s)",
                             csi_as_string(term, final));
                    break;

                case 4:
                    LOG_WARN("unimplemented: Smooth (Slow) Scroll (DECSCLM, %s)",
                             csi_as_string(term, final));
                    break;

                case 5:
                    term->reverse = false;
                    term_damage_all(term);
                    break;

                case 7:
                    term->auto_margin = false;
                    break;

                case 12:
                    /* Ignored */
                    break;

                case 25:
                    term->hide_cursor = true;
                    break;

                case 1000:  /* MOUSE_NORMAL */
                case 1002:  /* MOUSE_BUTTON_EVENT */
                case 1003:  /* MOUSE_ANY_EVENT */
                    term->mouse_tracking = MOUSE_NONE;
                    break;

                case 1005:  /* MOUSE_UTF8 */
                case 1006:  /* MOUSE_SGR */
                case 1015:  /* MOUSE_URXVT */
                    term->mouse_reporting = MOUSE_NORMAL;
                    break;

                case 1004:
                    term->focus_events = false;
                    break;

                case 1007:
                    term->alt_scrolling = false;
                    break;

                case 1036:
                    /* metaSendsEscape - we always send escape */
                    LOG_WARN("unimplemented: meta does *not* send escape");
                    break;

                case 1042:
                    /* 'urgency' window manager hint on ctrl-g */
                    break;

                case 1043:
                    /* raise window on ctrl-g */
                    break;

                case 1049:
                    if (term->grid == &term->alt) {
                        selection_cancel(term);

                        term->grid = &term->normal;
                        term_restore_cursor(term);

                        tll_free(term->alt.damage);
                        tll_free(term->alt.scroll_damage);

                        term_damage_all(term);
                    }
                    break;

                case 2004:
                    term->bracketed_paste = false;
                    break;

                default:
                    UNHANDLED();
                    break;
                }
            }
            break;
        }

        case 'p': {
            if (term->vt.private[1] != '$') {
                UNHANDLED();
                break;
            }

            unsigned param = vt_param_get(term, 0, 0);

            /*
             * Request DEC private mode (DECRQM)
             * Reply:
             *   0 - not recognized
             *   1 - set
             *   2 - reset
             *   3 - permanently set
             *   4 - permantently reset
             */
            char reply[32];
            snprintf(reply, sizeof(reply), "\033[?%u;2$y", param);
            term_to_slave(term, reply, strlen(reply));
            break;
        }

        case 's':
            for (size_t i = 0; i < term->vt.params.idx; i++) {
                switch (term->vt.params.v[i].value) {
                case 1001:  /* save old highlight mouse tracking mode? */
                    LOG_WARN(
                        "unimplemented: %s "
                        "(save 'highlight mouse tracking' mode)",
                        csi_as_string(term, final));
                    break;

                default:
                    UNHANDLED();
                    break;
                }
            }
            break;

        case 'r':
            for (size_t i = 0; i < term->vt.params.idx; i++) {
                switch (term->vt.params.v[i].value) {
                case 1001:  /* restore old highlight mouse tracking mode? */
                    LOG_WARN(
                        "unimplemented: %s "
                        "(restore 'highlight mouse tracking' mode)",
                        csi_as_string(term, final));
                    break;

                default:
                    UNHANDLED();
                    break;
                }
            }
            break;

        default:
            UNHANDLED();
            break;
        }

        break; /* private == '?' */
    }

    case '>': {
        switch (final) {
            case 'c': {
                int param = vt_param_get(term, 0, 0);
                if (param != 0) {
                    UNHANDLED();
                    break;
                }

                term_to_slave(term, "\033[>41;347;0c", 12);
                break;
            }

        case 'm':
            if (term->vt.params.idx == 0) {
                /* Reset all */
            } else {
                int resource = vt_param_get(term, 0, 0);
                int value = vt_param_get(term, 1, -1);

                switch (resource) {
                case 0: /* modifyKeyboard */
                    break;

                case 1: /* modifyCursorKeys */
                case 2: /* modifyFunctionKeys */
                case 4: /* modifyOtherKeys */
                    /* Ignored, we always report modifiers */
                    if (value != 2 && value != -1 &&
                        !(resource == 4 && value == 1))
                    {
                        LOG_WARN("unimplemented: %s = %d",
                                 resource == 1 ? "modifyCursorKeys" :
                                 resource == 2 ? "modifyFunctionKeys" :
                                 resource == 4 ? "modifyOtherKeys" : "<invalid>",
                                 value);
                    }
                    break;

                default:
                    LOG_WARN("invalid resource %d in %s",
                             resource, csi_as_string(term, final));
                    break;
                }
            }
            break; /* final == 'm' */

        default:
            UNHANDLED();
            break;
        }

        break; /* private == '>' */
    }

    case ' ': {
        switch (final) {
        case 'q': {
            int param = vt_param_get(term, 0, 0);
            switch (param) {
            case 0: case 1: /* blinking block */
                term->cursor_style = CURSOR_BLOCK;
                break;

            case 2:         /* steady block - but can be overriden in footrc */
                term->cursor_style = term->default_cursor_style;
                break;

            case 3:         /* blinking underline */
            case 4:         /* steady underline */
                term->cursor_style = CURSOR_UNDERLINE;
                break;

            case 5:         /* blinking bar */
            case 6:         /* steady bar */
                term->cursor_style = CURSOR_BAR;
                break;
            }

            term->cursor_blinking = param == 0 || param & 1;
            if (term->cursor_blinking)
                LOG_WARN("unimplemented: blinking cursor");
            break;
        }

        default:
            UNHANDLED();
            break;
        }
        break; /* private == ' ' */
    }

    case '!': {
        switch (final) {
        case 'p':
            term_reset(term, false);
            break;

        default:
            UNHANDLED();
            break;
        }
        break; /* private == '!' */
    }

    default:
        UNHANDLED();
        break;
    }
}
