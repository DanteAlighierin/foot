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

static const struct rgb colors_regular[] = {
    {0.000000, 0.000000, 0.000000},  /* 0x000000 */
    {0.800000, 0.576471, 0.576471},  /* 0xcc9393 */
    {0.498039, 0.623529, 0.498039},  /* 0x7f9f7f */
    {0.815686, 0.749020, 0.560784},  /* 0xd0bf8f */
    {0.423529, 0.627451, 0.639216},  /* 0x6ca0a3 */
    {0.862745, 0.549020, 0.764706},  /* 0xdc8cc3 */
    {0.576471, 0.878431, 0.890196},  /* 0x93e0e3 */
    {0.862745, 0.862745, 0.800000},  /* 0xdcdccc */
};

static const struct rgb colors_bright[] = {
    {0.000000, 0.000000, 0.000000},  /* 0x000000 */
    {0.862745, 0.639216, 0.639216},  /* 0xdca3a3 */
    {0.749020, 0.921569, 0.749020},  /* 0xbfebbf */
    {0.941176, 0.874510, 0.686275},  /* 0xf0dfaf */
    {0.549020, 0.815686, 0.827451},  /* 0x8cd0d3 */
    {0.862745, 0.549020, 0.764706},  /* 0xdc8cc3 */
    {0.576471, 0.878431, 0.890196},  /* 0x93e0e3 */
    {1.000000, 1.000000, 1.000000},  /* 0xffffff */
};

static struct rgb colors256[256];

static void __attribute__((constructor))
initialize_colors256(void)
{
    for (size_t i = 0; i < 8; i++)
        colors256[i] = colors_regular[i];
    for (size_t i = 0; i < 8; i++)
        colors256[8 + i] = colors_bright[i];

    for (size_t r = 0; r < 6; r++) {
        for (size_t g = 0; g < 6; g++) {
            for (size_t b = 0; b < 6; b++) {
                colors256[16 + r * 6 * 6 + g * 6 + b] = (struct rgb) {
                    r * 51 / 255.0,
                    g * 51 / 255.0,
                    b * 51 / 255.0,
                };
            }
        }
    }

    for (size_t i = 0; i < 24; i++){
        colors256[232 + i] = (struct rgb) {
            i * 11 / 255.0,
            i * 11 / 255.0,
            i * 11 / 255.0,
        };
    }
}

static void
sgr_reset(struct terminal *term)
{
    memset(&term->vt.attrs, 0, sizeof(term->vt.attrs));
    term->vt.dim = false;
    term->vt.attrs.foreground = term->foreground;
    term->vt.attrs.background = term->background;
}

static const char *
csi_as_string(struct terminal *term, uint8_t final)
{
    static char msg[1024];
    int c = snprintf(msg, sizeof(msg), "CSI: ");

    if (term->vt.private != 0)
        c += snprintf(&msg[c], sizeof(msg) - c, "%c", term->vt.private);

    for (size_t i = 0; i < term->vt.params.idx; i++){
        c += snprintf(&msg[c], sizeof(msg) - c, "%d",
                      term->vt.params.v[i].value);

        for (size_t j = 0; i < term->vt.params.v[i].sub.idx; j++) {
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
        case 2: term->vt.dim = true; break;
        case 3: term->vt.attrs.italic = true; break;
        case 4: term->vt.attrs.underline = true; break;
        case 5: LOG_WARN("unimplemented: %s (blink)", csi_as_string(term, 'm')); /*term->vt.attrs.blink = true; */break;
        case 6: break; /* rapid blink, ignored */
        case 7: term->vt.attrs.reverse = true; break;
        case 8: term->vt.attrs.conceal = true; break;
        case 9: term->vt.attrs.strikethrough = true; break;

        case 22: term->vt.attrs.bold = term->vt.dim = false; break;
        case 23: term->vt.attrs.italic = false; break;
        case 24: term->vt.attrs.underline = false; break;
        case 25: /*term->vt.attrs.blink = false; */break;
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
            term->vt.attrs.have_foreground = true;
            term->vt.attrs.foreground = colors_regular[param - 30];
            break;

        case 38: {
            if (term->vt.params.idx - i - 1 >= 2 &&
                term->vt.params.v[i + 1].value == 5)
            {
                size_t idx = term->vt.params.v[i + 2].value;
                term->vt.attrs.foreground = colors256[idx];
                term->vt.attrs.have_foreground = true;
                i += 2;

            }

            else if (term->vt.params.idx - i - 1 >= 4 &&
                     term->vt.params.v[i + 1].value == 2)
            {
                uint8_t r = term->vt.params.v[i + 2].value;
                uint8_t g = term->vt.params.v[i + 3].value;
                uint8_t b = term->vt.params.v[i + 4].value;
                term->vt.attrs.foreground = (struct rgb) {
                    r / 255.0,
                    g / 255.0,
                    b / 255.0,
                };
                term->vt.attrs.have_foreground = true;
                i += 4;
            }

            else if (term->vt.params.v[i].sub.idx > 0) {
                const struct vt_param *param = &term->vt.params.v[i];

                if (param->sub.value[0] == 2 && param->sub.idx >= 5) {
                    int color_space_id __attribute__((unused)) = param->sub.value[1];
                    uint8_t r = param->sub.value[2];
                    uint8_t g = param->sub.value[3];
                    uint8_t b = param->sub.value[4];
                    /* 5 - unused */
                    /* 6 - CS tolerance */
                    /* 7 - color space associated with tolerance */

                    term->vt.attrs.foreground = (struct rgb) {
                        r / 255.0,
                        g / 255.0,
                        b / 255.0,
                    };
                    term->vt.attrs.have_foreground = true;
                } else {
                    LOG_ERR("invalid CSI SGR sequence");
                    abort();
                }
            }

            else {
                LOG_ERR("invalid CSI SGR sequence");
                abort();
            }
            break;
        }

        case 39:
            term->vt.attrs.foreground = term->foreground;
            term->vt.attrs.have_foreground = false;
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
            term->vt.attrs.background = colors_regular[param - 40];
            term->vt.attrs.have_background = true;
            break;

        case 48: {
            if (term->vt.params.idx - i - 1 >= 2 &&
                term->vt.params.v[i + 1].value == 5)
            {
                size_t idx = term->vt.params.v[i + 2].value;
                term->vt.attrs.background = colors256[idx];
                term->vt.attrs.have_background = true;
                i += 2;
            }

            else if (term->vt.params.idx - i - 1 >= 4 &&
                       term->vt.params.v[i + 1].value == 2)
            {
                uint8_t r = term->vt.params.v[i + 2].value;
                uint8_t g = term->vt.params.v[i + 3].value;
                uint8_t b = term->vt.params.v[i + 4].value;
                term->vt.attrs.background = (struct rgb) {
                    r / 255.0,
                    g / 255.0,
                    b / 255.0,
                };
                term->vt.attrs.have_background = true;
                i += 4;

            }

            else if (term->vt.params.v[i].sub.idx > 0) {
                const struct vt_param *param = &term->vt.params.v[i];

                if (param->sub.value[0] == 2 && param->sub.idx >= 5) {
                    int color_space_id __attribute__((unused)) = param->sub.value[1];
                    uint8_t r = param->sub.value[2];
                    uint8_t g = param->sub.value[3];
                    uint8_t b = param->sub.value[4];
                    /* 5 - unused */
                    /* 6 - CS tolerance */
                    /* 7 - color space associated with tolerance */

                    term->vt.attrs.background = (struct rgb) {
                        r / 255.0,
                        g / 255.0,
                        b / 255.0,
                    };
                    term->vt.attrs.have_background = true;
                } else {
                    LOG_ERR("invalid CSI SGR sequence");
                    abort();
                }
            }

            else {
                LOG_ERR("invalid CSI SGR sequence");
                abort();
                break;
            }
            break;
        }
        case 49:
            term->vt.attrs.background = term->background;
            term->vt.attrs.have_background = false;
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
            term->vt.attrs.foreground = colors_bright[param - 90];
            term->vt.attrs.have_foreground = true;
            break;

        /* Regular background colors */
        case 100:
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            term->vt.attrs.background = colors_bright[param - 100];
            term->vt.attrs.have_background = true;
            break;

        default:
            LOG_ERR("unimplemented: CSI: SGR: %u", term->vt.params.v[i].value);
            abort();
            break;
        }
    }
}

void
csi_dispatch(struct terminal *term, uint8_t final)
{
    LOG_DBG("%s", csi_as_string(term, final));

    switch (term->vt.private) {
    case 0: {
        switch (final) {
        case 'c':
            vt_to_slave(term, "\033[?6c", 5);
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

            default:
                LOG_ERR("%s: invalid argument: %d",
                        csi_as_string(term, final), param);
                abort();
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
                LOG_ERR("%s: invalid argument: %d",
                        csi_as_string(term, final), param);
                abort();
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
            /* DCH: Delete character */

            /* Number of characters to delete */
            int count = min(
                vt_param_get(term, 0, 1), term->cols - term->cursor.col);

            /* Number of characters left after deletion (on current line) */
            int remaining = term->cols - (term->cursor.col + count);

            /* 'Delete' characters by moving the remaining ones */
            memmove(&term->grid->cur_row->cells[term->cursor.col],
                    &term->grid->cur_row->cells[term->cursor.col + count],
                    remaining * sizeof(term->grid->cur_row->cells[0]));
#if 0
            term_damage_update(term, term->cursor.linear, remaining);
#else
            term->grid->cur_row->dirty = true;
#endif

            /* Erase the remainder of the line */
            term_erase(
                term,
                &(struct coord){term->cursor.col + remaining, term->cursor.row},
                &(struct coord){term->cols - 1, term->cursor.row});
#if 0
            term_erase(
                term, term->cursor.linear + remaining,
                term->cursor.linear + remaining + count);
#endif
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

#if 0
            memset(&term->grid->cur_line[term->cursor.col],
                   0, count * sizeof(term->grid->cur_line[0]));
            term_damage_erase(term, term->cursor.linear, count);
#endif
            break;
        }

        case 'h':
            /* smir - insert mode enable */
            assert(false && "untested");
            term->insert_mode = true;
            break;

        case 'l':
            /* rmir - insert mode disable */
            term->insert_mode = false;
            break;

        case 'r': {
            int start = vt_param_get(term, 0, 1);
            int end = min(vt_param_get(term, 1, term->rows), term->rows);

            /* 1-based */
            term->scroll_region.start = start - 1;
            term->scroll_region.end = end;

            term_cursor_to(term, start - 1, 0);

            LOG_DBG("scroll region: %d-%d",
                    term->scroll_region.start,
                    term->scroll_region.end);
            break;
        }

        case 't':
            /*
             * TODO: xterm's terminfo specifies *both* \e[?1049h *and*
             * \e[22;0;0t in smcup, but only one is necessary. We
             * should provide our own terminfo with *only* \e[?1049h
             * (and \e[?1049l for rmcup)
             */
            LOG_WARN("ignoring %s", csi_as_string(term, final));
            break;

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
                    vt_to_slave(term, reply, strlen(reply));
                    break;
                }

                default:
                    LOG_ERR("unimplemented: %s, parameter = %d",
                            csi_as_string(term, final), param);
                    abort();
                    break;
                }
            } else {
                LOG_ERR("%s: missing parameter", csi_as_string(term, final));
                abort();
            }
            break;
        }

        case '=':
            /*
             * TODO: xterm's terminfo specifies *both* \e[?1h *and*
             * \e= in smkx, but only one is necessary. We should
             * provide our own terminfo with *only* \e[?1h (and \e[?1l
             * for rmkx)
             */
            LOG_WARN("ignoring CSI with final '='");
            break;

        default:
            LOG_ERR("unimplemented: %s", csi_as_string(term, final));
            abort();
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

                case 1015:
                    term->mouse_reporting = MOUSE_URXVT;
                    break;

                case 1049:
                    if (term->grid != &term->alt) {
                        term->grid = &term->alt;
                        term->saved_cursor = term->cursor;

                        term_cursor_to(term, term->cursor.row, term->cursor.col);

                        tll_free(term->alt.damage);
                        tll_free(term->alt.scroll_damage);
                        selection_cancel(term);

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
                    LOG_ERR("%s: unimplemented param: %d",
                            csi_as_string(term, final),
                            term->vt.params.v[i].value);
                    abort();
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
                case 1005:  /* MOUSE_UTF8 */
                case 1006:  /* MOUSE_SGR */
                case 1015:  /* MOUSE_URXVT */
                    term->mouse_tracking = MOUSE_NONE;
                    break;

                case 1004:
                    term->focus_events = false;
                    break;

                case 1049:
                    if (term->grid == &term->alt) {
                        term->grid = &term->normal;

                        term->cursor = term->saved_cursor;
                        term_cursor_to(term, term->cursor.row, term->cursor.col);

                        tll_free(term->alt.damage);
                        tll_free(term->alt.scroll_damage);

                        selection_cancel(term);
                        term_damage_all(term);
                    }
                    break;

                case 2004:
                    term->bracketed_paste = false;
                    break;

                default:
                    LOG_ERR("%s: unimplemented param: %d",
                            csi_as_string(term, final),
                            term->vt.params.v[i].value);
                    abort();
                    break;
                }
            }
            break;
        }

        case 's':
            for (size_t i = 0; i < term->vt.params.idx; i++) {
                switch (term->vt.params.v[i].value) {
                case 1001:  /* save old highlight mouse tracking mode? */
                    LOG_WARN(
                        "unimplemented: CSI ?1001s "
                        "(save 'highlight mouse tracking' mode)");
                    break;

                default:
                    LOG_ERR("unimplemented: CSI %s", csi_as_string(term, final));
                    abort();
                }
            }
            break;

        case 'r':
            for (size_t i = 0; i < term->vt.params.idx; i++) {
                switch (term->vt.params.v[i].value) {
                case 1001:  /* restore old highlight mouse tracking mode? */
                    LOG_WARN(
                        "unimplemented: CSI ?1001r "
                        "(restore 'highlight mouse tracking' mode)");
                    break;

                default:
                    LOG_ERR("unimplemented: CSI %s", csi_as_string(term, final));
                    abort();
                }
            }
            break;

        default:
            LOG_ERR("unimplemented: %s", csi_as_string(term, final));
            abort();
        }

        break; /* private == '?' */
    }

    case '>': {
        switch (final) {
            case 'c': {
                int param = vt_param_get(term, 0, 0);
                if (param != 0) {
                    LOG_ERR(
                        "unimplemented: send device attributes with param = %d",
                        param);
                    abort();
                    break;
                }

                vt_to_slave(term, "\033[?6c", 5);
                break;
            }

        default:
            LOG_ERR("unimplemented: %s", csi_as_string(term, final));
            abort();
            break;
        }

        break; /* private == '>' */
    }

    case ' ': {
        switch (final) {
        case 'q': {
            int param = vt_param_get(term, 0, 0);
            switch (param) {
            case 2: /* steady block */
                break;

            case 0:
            case 1: /* blinking block */
            case 3: /* blinking underline */
            case 4: /* steady underline */
            case 5: /* blinking bar */
            case 6: /* steady bar */
                LOG_WARN("unimplemented: cursor style: %s",
                         param == 0 || param == 1 ? "blinking block" :
                         param == 3 ? "blinking underline" :
                         param == 4 ? "steady underline" :
                         param == 5 ? "blinking bar" : "steady bar");
                break;
            }
            break;
        }

        default:
            LOG_ERR("unimplemented: %s", csi_as_string(term, final));
            abort();
            break;
        }
        break; /* private == ' ' */
    }

    default:
        LOG_ERR("unimplemented: %s", csi_as_string(term, final));
        abort();
        break;
    }
}
