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

static const uint32_t colors_regular[] = {
    0x000000ff,
    0xcc9393ff,
    0x7f9f7fff,
    0xd0bf8fff,
    0x6ca0a3ff,
    0xdc8cc3ff,
    0x93e0e3ff,
    0xdcdcccff,
};

static const uint32_t colors_bright[] = {
    0x000000ff,
    0xdca3a3ff,
    0xbfebbfff,
    0xf0dfafff,
    0x8cd0d3ff,
    0xdc8cc3ff,
    0x93e0e3ff,
    0xffffffff,
};

static uint32_t colors256[256];

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
                colors256[16 + r * 6 * 6 + g * 6 + b] =
                    (51 * r) << 24 | (51 * g) << 16 | (51 * b) << 8 | 0xff;
            }
        }
    }

    for (size_t i = 0; i < 24; i++)
        colors256[232 + i] = (11 * i) << 24 | (11 * i) << 16 | (11 * i) << 8 | 0xff;
}

static void
sgr_reset(struct terminal *term)
{
    term->vt.attrs.bold = false;
    term->vt.dim = false;
    term->vt.attrs.italic = false;
    term->vt.attrs.underline = false;
    term->vt.attrs.strikethrough = false;
    term->vt.attrs.blink = false;
    term->vt.attrs.conceal = false;
    term->vt.attrs.reverse = false;
    term->vt.attrs.foreground = term->grid.foreground;
    term->vt.attrs.background = term->grid.background;
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
            term->vt.attrs.foreground = colors_regular[param - 30];
            break;

        case 38: {
            if (term->vt.params.idx - i - 1 == 2 &&
                term->vt.params.v[i + 1].value == 5)
            {
                size_t idx = term->vt.params.v[i + 2].value;
                term->vt.attrs.foreground = colors256[idx];
                i += 2;
            } else if (term->vt.params.idx - i - 1 == 4 &&
                       term->vt.params.v[i + 1].value == 2)
            {
                uint32_t r = term->vt.params.v[i + 2].value;
                uint32_t g = term->vt.params.v[i + 3].value;
                uint32_t b = term->vt.params.v[i + 4].value;
                term->vt.attrs.foreground = r << 24 | g << 16 | b << 8 | 0xff;
                i += 4;
            } else {
                LOG_ERR("invalid CSI SGR sequence");
                return false;
            }
            break;
        }
        case 39: term->vt.attrs.foreground = term->grid.foreground; break;

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
            break;

        case 48: {
            if (term->vt.params.idx - i - 1 == 2 &&
                term->vt.params.v[i + 1].value == 5)
            {
                size_t idx = term->vt.params.v[i + 2].value;
                term->vt.attrs.background = colors256[idx];
                i += 2;
            } else if (term->vt.params.idx - i - 1 == 4 &&
                       term->vt.params.v[i + 1].value == 2)
            {
                uint32_t r = term->vt.params.v[i + 2].value;
                uint32_t g = term->vt.params.v[i + 3].value;
                uint32_t b = term->vt.params.v[i + 4].value;
                term->vt.attrs.background = r << 24 | g << 16 | b << 8 | 0xff;
                i += 4;
            } else {
                LOG_ERR("invalid CSI SGR sequence");
                return false;
            }
            break;
        }
        case 49: term->vt.attrs.background = term->grid.background; break;

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
#if defined(_DEBUG)
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

    c += snprintf(&log[c], sizeof(log) - c, "%c (%zu parameters)",
                  final, term->vt.params.idx);
    LOG_DBG("%s", log);
#endif

    if (term->vt.intermediates.idx == 0) {
        switch (final) {
        case 'c':
            return write(term->ptmx, "\033[?6c", 5) == 5;

        case 'm':
            return csi_sgr(term);

        case 'A': {
            int count = term->vt.params.idx > 0 ? term->vt.params.v[0].value : 1;
            grid_cursor_up(&term->grid, count);
            break;
        }

        case 'B': {
            int count = term->vt.params.idx > 0 ? term->vt.params.v[0].value : 1;
            grid_cursor_down(&term->grid, count);
            break;
        }

        case 'C': {
            int count = term->vt.params.idx > 0 ? term->vt.params.v[0].value : 1;
            grid_cursor_right(&term->grid, count);
            break;
        }

        case 'D': {
            int count = term->vt.params.idx > 0 ? term->vt.params.v[0].value : 1;
            grid_cursor_left(&term->grid, count);
            break;
        }

        case 'H': {
            /* Move cursor */
            int row = term->vt.params.idx > 0 ? term->vt.params.v[0].value : 1;
            int col = term->vt.params.idx > 1 ? term->vt.params.v[1].value : 1;

            grid_cursor_to(&term->grid, row - 1, col - 1);
            break;
        }

        case 'J': {
            /* Erase screen */

            int param = 0;
            if (term->vt.params.idx > 0)
                param = term->vt.params.v[0].value;

            int start = -1;
            int end = -1;
            switch (param) {
            case 0:
                /* From cursor to end of screen */
                start = term->grid.linear_cursor;
                end = term->grid.cols * term->grid.rows;
                break;

            case 1:
                /* From start of screen to cursor */
                start = 0;
                end = term->grid.linear_cursor;
                break;

            case 2:
                /* Erase entire screen */
                start = 0;
                end = term->grid.cols * term->grid.rows;
                break;

            default:
                LOG_ERR("CSI: J: invalid argument: %d", param);
                return false;
            }

            grid_erase(&term->grid, start, end);
            break;
        }

        case 'K': {
            /* Erase line */

            int param = term->vt.params.idx > 0 ? term->vt.params.v[0].value : 0;
            int start = -1;
            int end = -1;
            switch (param) {
            case 0:
                /* From cursor to end of line */
                start = term->grid.linear_cursor;
                end = grid_cursor_linear(
                    &term->grid, term->grid.cursor.row, term->grid.cols);
                break;

            case 1:
                /* From start of line to cursor */
                start = grid_cursor_linear(
                    &term->grid, term->grid.cursor.row, 0);
                end = term->grid.linear_cursor;
                break;

            case 2:
                /* Entire line */
                start = grid_cursor_linear(
                    &term->grid, term->grid.cursor.row, 0);
                end = grid_cursor_linear(
                    &term->grid, term->grid.cursor.row, term->grid.cols);
                break;

            default:
                LOG_ERR("CSI: K: invalid argument: %d", param);
                return false;
            }

            grid_erase(&term->grid, start, end);
            break;
        }

        case 'P': {
            /* DCH: Delete character */
            int param = term->vt.params.idx > 0 ? term->vt.params.v[0].value : 1;

            /* Only delete up to the right margin */
            const int max_end = grid_cursor_linear(
                &term->grid, term->grid.cursor.row, term->grid.cols);

            int start = term->grid.linear_cursor;
            int end = min(start + param, max_end);

            /* Erase the requested number of characters */
            grid_erase(&term->grid, start, end);

            /* Move remaining (up til the right margin) characters */
            int count = max_end - end;
            memmove(&term->grid.cells[start],
                    &term->grid.cells[end],
                    count * sizeof(term->grid.cells[0]));
            grid_damage_update(&term->grid, term->grid.linear_cursor, count);
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

                case 1049:
                    if (term->grid.cells != term->grid.alt_grid) {
                        term->grid.cells = term->grid.alt_grid;

                        term->grid.alt_saved_cursor.row = term->grid.cursor.row;
                        term->grid.alt_saved_cursor.col = term->grid.cursor.col;

                        tll_free(term->grid.damage);
                        grid_erase(&term->grid, 0, term->grid.cols * term->grid.rows);
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

                case 1049:
                    if (term->grid.cells == term->grid.alt_grid) {
                        term->grid.cells = term->grid.normal_grid;

                        term->grid.cursor.row = term->grid.alt_saved_cursor.row;
                        term->grid.cursor.col = term->grid.alt_saved_cursor.col;
                        term->grid.linear_cursor = grid_cursor_linear(
                            &term->grid, term->grid.cursor.row, term->grid.cursor.col);

                        tll_free(term->grid.damage);
                        grid_damage_update(
                            &term->grid, 0, term->grid.cols * term->grid.rows);
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

    else {
        LOG_ERR("CSI: unimplemented: intermediates: %.*s",
                (int)term->vt.intermediates.idx,
                term->vt.intermediates.data);
        abort();
    }

    return false;
}
