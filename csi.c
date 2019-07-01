#include "csi.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#if defined(_DEBUG)
 #include <stdio.h>
#endif

#define LOG_MODULE "csi"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "grid.h"

#define min(x, y) ((x) < (y) ? (x) : (y))

static const struct rgba colors_regular[] = {
    {0.000000, 0.000000, 0.000000, 1.000000},  /* 0x000000ff */
    {0.800000, 0.576471, 0.576471, 1.000000},  /* 0xcc9393ff */
    {0.498039, 0.623529, 0.498039, 1.000000},  /* 0x7f9f7fff */
    {0.815686, 0.749020, 0.560784, 1.000000},  /* 0xd0bf8fff */
    {0.423529, 0.627451, 0.639216, 1.000000},  /* 0x6ca0a3ff */
    {0.862745, 0.549020, 0.764706, 1.000000},  /* 0xdc8cc3ff */
    {0.576471, 0.878431, 0.890196, 1.000000},  /* 0x93e0e3ff */
    {0.862745, 0.862745, 0.800000, 1.000000},  /* 0xdcdcccff */
};

static const struct rgba colors_bright[] = {
    {0.000000, 0.000000, 0.000000, 1.000000},  /* 0x000000ff */
    {0.862745, 0.639216, 0.639216, 1.000000},  /* 0xdca3a3ff */
    {0.749020, 0.921569, 0.749020, 1.000000},  /* 0xbfebbfff */
    {0.941176, 0.874510, 0.686275, 1.000000},  /* 0xf0dfafff */
    {0.549020, 0.815686, 0.827451, 1.000000},  /* 0x8cd0d3ff */
    {0.862745, 0.549020, 0.764706, 1.000000},  /* 0xdc8cc3ff */
    {0.576471, 0.878431, 0.890196, 1.000000},  /* 0x93e0e3ff */
    {1.000000, 1.000000, 1.000000, 1.000000},  /* 0xffffffff */
};

static struct rgba colors256[256];

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
                colors256[16 + r * 6 * 6 + g * 6 + b] = (struct rgba) {
                    r * 51 / 255.0,
                    g * 51 / 255.0,
                    b * 51 / 255.0,
                    1.0,
                };
#if 0
                colors256[16 + r * 6 * 6 + g * 6 + b] =
                    (51 * r) << 24 | (51 * g) << 16 | (51 * b) << 8 | 0xff;
#endif
            }
        }
    }

    for (size_t i = 0; i < 24; i++){
        colors256[232 + i] = (struct rgba) {
            i * 11 / 255.0,
            i * 11 / 255.0,
            i * 11 / 255.0,
            1.0
        };
            //(11 * i) << 24 | (11 * i) << 16 | (11 * i) << 8 | 0xff;
    }
}

static int
param_get(const struct terminal *term, size_t idx, int default_value)
{
    if (term->vt.params.idx > idx) {
        int value = term->vt.params.v[idx].value;
        return value != 0 ? value : default_value;
    }

    return default_value;
}

static void
sgr_reset(struct terminal *term)
{
    memset(&term->vt.attrs, 0, sizeof(term->vt.attrs));
    term->vt.dim = false;
    term->vt.attrs.foreground = term->foreground;
    term->vt.attrs.background = term->background;
}

static bool
csi_sgr(struct terminal *term)
{
    if (term->vt.params.idx == 0) {
        sgr_reset(term);
        return true;
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
        case 5: term->vt.attrs.blink = true; break;
        case 6: term->vt.attrs.blink = true; break;
        case 7: term->vt.attrs.reverse = true; break;
        case 8: term->vt.attrs.conceal = true; break;
        case 9: term->vt.attrs.strikethrough = true; break;

        case 22: term->vt.attrs.bold = term->vt.dim = false; break;
        case 23: term->vt.attrs.italic = false; break;
        case 24: term->vt.attrs.underline = false; break;
        case 25: term->vt.attrs.blink = false; break;
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
            if (term->vt.params.idx - i - 1 == 2 &&
                term->vt.params.v[i + 1].value == 5)
            {
                size_t idx = term->vt.params.v[i + 2].value;
                term->vt.attrs.foreground = colors256[idx];
                term->vt.attrs.have_foreground = true;
                i += 2;
            } else if (term->vt.params.idx - i - 1 == 4 &&
                       term->vt.params.v[i + 1].value == 2)
            {
                uint8_t r = term->vt.params.v[i + 2].value;
                uint8_t g = term->vt.params.v[i + 3].value;
                uint8_t b = term->vt.params.v[i + 4].value;
                term->vt.attrs.foreground = (struct rgba) {
                    r / 255.0,
                    g / 255.0,
                    b / 255.0,
                    1.0,
                };
                term->vt.attrs.have_foreground = true;
                i += 4;
            } else {
                LOG_ERR("invalid CSI SGR sequence");
                return false;
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
            if (term->vt.params.idx - i - 1 == 2 &&
                term->vt.params.v[i + 1].value == 5)
            {
                size_t idx = term->vt.params.v[i + 2].value;
                term->vt.attrs.background = colors256[idx];
                term->vt.attrs.have_background = true;
                i += 2;
            } else if (term->vt.params.idx - i - 1 == 4 &&
                       term->vt.params.v[i + 1].value == 2)
            {
                uint8_t r = term->vt.params.v[i + 2].value;
                uint8_t g = term->vt.params.v[i + 3].value;
                uint8_t b = term->vt.params.v[i + 4].value;
                term->vt.attrs.background = (struct rgba) {
                    r / 255.0,
                    g / 255.0,
                    b / 255.0,
                    1.0
                };
                term->vt.attrs.have_background = true;
                i += 4;
            } else {
                LOG_ERR("invalid CSI SGR sequence");
                return false;
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
            return false;
        }
    }

    return true;
}

bool
csi_dispatch(struct terminal *term, uint8_t final)
{
#if defined(_DEBUG) && defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    char log[1024];
    int c = snprintf(log, sizeof(log), "CSI: ");

    for (size_t i = 0; i < term->vt.intermediates.idx; i++)
        c += snprintf(&log[c], sizeof(log) - c, "%c", term->vt.intermediates.data[i]);

    for (size_t i = 0; i < term->vt.params.idx; i++){
        c += snprintf(&log[c], sizeof(log) - c, "%d",
                      term->vt.params.v[i].value);

        for (size_t j = 0; i < term->vt.params.v[i].sub.idx; j++) {
            c += snprintf(&log[c], sizeof(log) - c, ":%d",
                          term->vt.params.v[i].sub.value[j]);
        }

        c += snprintf(&log[c], sizeof(log) - c, "%s",
                      i == term->vt.params.idx - 1 ? "" : ";");
    }

    c += snprintf(&log[c], sizeof(log) - c, "%c (%d parameters)",
                  final, term->vt.params.idx);
    LOG_DBG("%s", log);
#endif

    if (term->vt.intermediates.idx == 0) {
        switch (final) {
        case 'c':
            return write(term->ptmx, "\033[?6c", 5) == 5;

        case 'd': {
            /* VPA - vertical line position absolute */
            int row = param_get(term, 0, 1);

            if (row > term->rows)
                row = term->rows;

            term_cursor_to(term, row - 1, term->cursor.col);
            break;
        }

        case 'm':
            return csi_sgr(term);

        case 'A':
            term_cursor_up(term, param_get(term, 0, 1));
            break;

        case 'e':
        case 'B':
            term_cursor_down(term, param_get(term, 0, 1));
            break;

        case 'C':
            term_cursor_right(term, param_get(term, 0, 1));
            break;

        case 'D':
            term_cursor_left(term, param_get(term, 0, 1));
            break;

        case 'G': {
            /* Cursor horizontal absolute */
            int col = param_get(term, 0, 1);

            if (col > term->cols)
                col = term->cols;

            term_cursor_to(term, term->cursor.row, col);
            break;
        }

        case 'H': {
            /* Move cursor */
            int row = param_get(term, 0, 1);
            int col = param_get(term, 1, 1);

            if (row > term->rows)
                row = term->rows;
            if (col > term->cols)
                col = term->cols;

            term_cursor_to(term, row - 1, col - 1);
            break;
        }

        case 'J': {
            /* Erase screen */

            int param = param_get(term, 0, 0);
            int start = -1;
            int end = -1;
            switch (param) {
            case 0:
                /* From cursor to end of screen */
                start = term->cursor.linear;
                end = term->cols * term->rows;
                break;

            case 1:
                /* From start of screen to cursor */
                start = 0;
                end = term->cursor.linear;
                break;

            case 2:
                /* Erase entire screen */
                start = 0;
                end = term->cols * term->rows;
                break;

            default:
                LOG_ERR("CSI: J: invalid argument: %d", param);
                return false;
            }

            term_erase(term, start, end);
            break;
        }

        case 'K': {
            /* Erase line */

            int param = param_get(term, 0, 0);
            int start = -1;
            int end = -1;
            switch (param) {
            case 0:
                /* From cursor to end of line */
                start = term->cursor.linear;
                end = term_cursor_linear(term, term->cursor.row, term->cols);
                break;

            case 1:
                /* From start of line to cursor */
                start = term_cursor_linear(term, term->cursor.row, 0);
                end = term->cursor.linear;
                break;

            case 2:
                /* Entire line */
                start = term_cursor_linear(term, term->cursor.row, 0);
                end = term_cursor_linear(term, term->cursor.row, term->cols);
                break;

            default:
                LOG_ERR("CSI: K: invalid argument: %d", param);
                return false;
            }

            term_erase(term, start, end);
            break;
        }

        case 'L': {
            if (term->cursor.row < term->scroll_region.start ||
                term->cursor.row >= term->scroll_region.end)
                break;

            int count = min(
                param_get(term, 0, 1),
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
                param_get(term, 0, 1),
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
            int count = param_get(term, 0, 1);

            /* Only delete up to the right margin */
            const int max_end = term_cursor_linear(
                term, term->cursor.row, term->cols);

            int start = term->cursor.linear;
            int end = min(start + count, max_end);

            /* Erase the requested number of characters */
            term_erase(term, start, end);

            /* Move remaining (up til the right margin) characters */
            int remaining = max_end - end;
            memmove(&term->grid->cells[start],
                    &term->grid->cells[end],
                    remaining * sizeof(term->grid->cells[0]));
            term_damage_update(term, term->cursor.linear, remaining);
            break;
        }

        case 'r': {
            int start = param_get(term, 0, 1);
            int end = param_get(term, 1, term->rows);

            /* 1-based */
            term->scroll_region.start = start - 1;
            term->scroll_region.end = end;

            term_cursor_to(term, (start - 1) * term->cols, 0);

            LOG_INFO("scroll region: %d-%d",
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
            LOG_WARN("ignoring CSI with final 't'");
            break;

        case 'n': {
            if (term->vt.params.idx > 0) {
                int param = param_get(term, 0, 0);
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
                    write(term->ptmx, reply, strlen(reply));
                    break;
                }

                default:
                    LOG_ERR("CSI: unimplemented parameter for 'n': %d", param);
                    return false;
                }

                return true;
            } else {
                LOG_ERR("CSI: missing parameter for 'n'");
                return false;
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
            LOG_ERR("CSI: unimplemented final: %c", final);
            abort();
        }

        return true;
    }

    else if (term->vt.intermediates.idx == 1 &&
               term->vt.intermediates.data[0] == '?') {

        switch (final) {
        case 'h': {
            for (size_t i = 0; i < term->vt.params.idx; i++) {
                switch (term->vt.params.v[i].value) {
                case 1:
                    term->decckm = DECCKM_SS3;
                    break;

                case 5:
                    LOG_WARN("unimplemented: flash");
                    break;

                case 12:
                    LOG_WARN("unimplemented: cursor blinking");
                    break;

                case 25:
                    LOG_WARN("unimplemented: civis");
                    break;

                case 1000:
                    LOG_WARN("unimplemented: report mouse clicks");
                    break;

                case 1002:
                    LOG_WARN("unimplemented: report cell mouse motion");
                    break;

                case 1006:
                    LOG_WARN("unimplemented: SGR mouse");
                    break;

                case 1049:
                    if (term->grid != &term->alt) {
                        term->grid = &term->alt;
                        term->saved_cursor = term->cursor;
                        term_damage_all(term);
                    }
                    break;

                case 2004:
                    term->bracketed_paste = true;
                    break;

                default:
                    LOG_ERR("CSI: 'h' (set mode): unimplemented param: %d",
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
                    term->decckm = DECCKM_CSI;
                    break;

                case 5:
                    LOG_WARN("unimplemented: flash");
                    break;

                case 12:
                    LOG_WARN("unimplemented: cursor blinking");
                    break;

                case 25:
                    LOG_WARN("unimplemented: civis");
                    break;

                case 1000:
                    LOG_WARN("unimplemented: report mouse clicks");
                    break;

                case 1002:
                    LOG_WARN("unimplemented: report cell mouse motion");
                    break;

                case 1006:
                    LOG_WARN("unimplemented: SGR mouse");
                    break;

                case 1049:
                    if (term->grid == &term->alt) {
                        term->grid = &term->normal;

                        term->cursor = term->saved_cursor;

                        /* Should these be restored from saved values? */
                        term->scroll_region.start = 0;
                        term->scroll_region.end = term->rows;

                        term_damage_all(term);
                    }
                    break;

                case 2004:
                    term->bracketed_paste = false;
                    break;

                default:
                    LOG_ERR("CSI: 'h' (unset mode): unimplemented param: %d",
                            term->vt.params.v[i].value);
                    abort();
                    break;
                }
            }
            break;
        }

        default:
            LOG_ERR("CSI: intermediate '?': unimplemented final: %c", final);
            abort();
        }

        return true;
    }

    else if (term->vt.intermediates.idx == 1 &&
               term->vt.intermediates.data[0] == '>') {
        switch (final) {
            case 'c': {
                int param = param_get(term, 0, 0);
                if (param != 0) {
                    LOG_ERR(
                        "unimplemented: send device attributes with param = %d",
                        param);
                    return false;
                }

                return write(term->ptmx, "\033[?6c", 5) == 5;
            }

        default:
            LOG_ERR("CSI: intermediate '>': unimplemented final: %c", final);
            abort();
        }
    }

    else {
        LOG_ERR("CSI: unimplemented: intermediates: %.*s",
                (int)term->vt.intermediates.idx,
                term->vt.intermediates.data);
        abort();
    }

    return false;
}
