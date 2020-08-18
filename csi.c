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
#include "selection.h"
#include "sixel.h"
#include "util.h"
#include "version.h"
#include "vt.h"
#include "xmalloc.h"

#define UNHANDLED()        LOG_DBG("unhandled: %s", csi_as_string(term, final, -1))
#define UNHANDLED_SGR(idx) LOG_DBG("unhandled: %s", csi_as_string(term, 'm', idx))

static void
sgr_reset(struct terminal *term)
{
    memset(&term->vt.attrs, 0, sizeof(term->vt.attrs));
    term->vt.attrs.fg = term->colors.fg;
    term->vt.attrs.bg = term->colors.bg;
}

static const char *
csi_as_string(struct terminal *term, uint8_t final, int idx)
{
    static char msg[1024];
    int c = snprintf(msg, sizeof(msg), "CSI: ");

    for (size_t i = idx >= 0 ? idx : 0;
         i < (idx >= 0 ? idx + 1 : term->vt.params.idx);
         i++)
    {
        c += snprintf(&msg[c], sizeof(msg) - c, "%u",
                      term->vt.params.v[i].value);

        for (size_t j = 0; j < term->vt.params.v[i].sub.idx; j++) {
            c += snprintf(&msg[c], sizeof(msg) - c, ":%u",
                          term->vt.params.v[i].sub.value[j]);
        }

        c += snprintf(&msg[c], sizeof(msg) - c, "%s",
                      i == term->vt.params.idx - 1 ? "" : ";");
    }

    for (size_t i = 0; i < sizeof(term->vt.private) / sizeof(term->vt.private[0]); i++) {
        if (term->vt.private[i] == 0)
            break;
        c += snprintf(&msg[c], sizeof(msg) - c, "%c", term->vt.private[i]);
    }

    snprintf(&msg[c], sizeof(msg) - c, "%c (%u parameters)",
             final, idx >= 0 ? 1 : term->vt.params.idx);
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
                        UNHANDLED_SGR(i);
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
                        UNHANDLED_SGR(i);
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
                    UNHANDLED_SGR(i);
                    break;
                }
            }

            /* Unrecognized */
            else
                UNHANDLED_SGR(i);

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
                        UNHANDLED_SGR(i);
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
                        UNHANDLED_SGR(i);
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
                    UNHANDLED_SGR(i);
                    break;
                }
            }

            else
                UNHANDLED_SGR(i);

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
            UNHANDLED_SGR(i);
            break;
        }
    }
}

static void
decset_decrst(struct terminal *term, unsigned param, bool enable)
{
#if defined(_DEBUG)
    /* For UNHANDLED() */
    int UNUSED final = enable ? 'h' : 'l';
#endif

    /* Note: update XTSAVE/XTRESTORE if adding/removing things here */

    switch (param) {
    case 1:
        /* DECCKM */
        term->cursor_keys_mode =
            enable ? CURSOR_KEYS_APPLICATION : CURSOR_KEYS_NORMAL;
        break;

    case 3:
        /* DECCOLM */
        if (enable)
            LOG_WARN("unimplemented: 132 column mode (DECCOLM)");

        term_erase(
            term,
            &(struct coord){0, 0},
            &(struct coord){term->cols - 1, term->rows - 1});
        term_cursor_home(term);
        break;

    case 4:
        /* DECSCLM - Smooth scroll */
        if (enable)
            LOG_WARN("unimplemented: Smooth (Slow) Scroll (DECSCLM)");
        break;

    case 5:
        /* DECSCNM */
        term->reverse = enable;
        term_damage_all(term);
        break;

    case 6: {
        /* DECOM */
        term->origin = enable ? ORIGIN_RELATIVE : ORIGIN_ABSOLUTE;
        term_cursor_home(term);
        break;
    }

    case 7:
        /* DECAWM */
        term->auto_margin = enable;
        term->grid->cursor.lcf = false;
        break;

    case 9:
        if (enable)
            LOG_WARN("unimplemented: X10 mouse tracking mode");
#if 0
        else if (term->mouse_tracking == MOUSE_X10)
            term->mouse_tracking = MOUSE_NONE;
#endif
        break;

    case 12:
        if (enable)
            term_cursor_blink_enable(term);
        else
            term_cursor_blink_disable(term);
        break;

    case 25:
        /* DECTCEM */
        term->hide_cursor = !enable;
        break;

    case 1000:
        if (enable)
            term->mouse_tracking = MOUSE_CLICK;
        else if (term->mouse_tracking == MOUSE_CLICK)
            term->mouse_tracking = MOUSE_NONE;
        term_xcursor_update(term);
        break;

    case 1001:
        if (enable)
            LOG_WARN("unimplemented: highlight mouse tracking");
        break;

    case 1002:
        if (enable)
            term->mouse_tracking = MOUSE_DRAG;
        else if (term->mouse_tracking == MOUSE_DRAG)
             term->mouse_tracking = MOUSE_NONE;
        term_xcursor_update(term);
        break;

    case 1003:
        if (enable)
            term->mouse_tracking = MOUSE_MOTION;
        else if (term->mouse_tracking == MOUSE_MOTION)
            term->mouse_tracking = MOUSE_NONE;
        term_xcursor_update(term);
        break;

    case 1004:
        term->focus_events = enable;
        break;

    case 1005:
        if (enable)
            LOG_WARN("unimplemented: mouse reporting mode: UTF-8");
#if 0
        else if (term->mouse_reporting == MOUSE_UTF8)
            term->mouse_reporting = MOUSE_NONE;
#endif
        break;

    case 1006:
        if (enable)
            term->mouse_reporting = MOUSE_SGR;
        else if (term->mouse_reporting == MOUSE_SGR)
            term->mouse_reporting = MOUSE_NORMAL;
        break;

    case 1007:
        term->alt_scrolling = enable;
        break;

    case 1015:
        if (enable)
            term->mouse_reporting = MOUSE_URXVT;
        else if (term->mouse_reporting == MOUSE_URXVT)
            term->mouse_reporting = MOUSE_NORMAL;
        break;

    case 1034:
        /* smm */
        LOG_DBG("%s 8-bit meta mode", enable ? "enabling" : "disabling");
        term->meta.eight_bit = enable;
        break;

    case 1036:
        /* metaSendsEscape */
        LOG_DBG("%s meta-sends-escape", enable ? "enabling" : "disabling");
        term->meta.esc_prefix = enable;
        break;

#if 0
    case 1042:
        LOG_WARN("unimplemented: 'urgency' window manager hint on ctrl-g");
        break;

    case 1043:
        LOG_WARN("unimplemented: raise window on ctrl-g");
        break;
#endif

    case 1049:
        if (enable && term->grid != &term->alt) {
            selection_cancel(term);

            term->grid = &term->alt;

            term_cursor_to(
                term,
                min(term->grid->cursor.point.row, term->rows - 1),
                min(term->grid->cursor.point.col, term->cols - 1));

            tll_free(term->alt.scroll_damage);

            term_erase(
                term,
                &(struct coord){0, 0},
                &(struct coord){term->cols - 1, term->rows - 1});
        }

        else if (!enable && term->grid == &term->alt) {
            selection_cancel(term);

            term->grid = &term->normal;

            term_cursor_to(
                term,
                min(term->grid->cursor.point.row, term->rows - 1),
                min(term->grid->cursor.point.col, term->cols - 1));

            tll_free(term->alt.scroll_damage);

            /* Delete all sixel images on the alt screen */
            tll_foreach(term->alt.sixel_images, it) {
                sixel_destroy(&it->item);
                tll_remove(term->alt.sixel_images, it);
            }

            term_damage_all(term);
        }
        break;

    case 2004:
        term->bracketed_paste = enable;
        break;

    default:
        UNHANDLED();
        break;
    }
}

static void
decset(struct terminal *term, unsigned param)
{
    decset_decrst(term, param, true);
}

static void
decrst(struct terminal *term, unsigned param)
{
    decset_decrst(term, param, false);
}

static void
xtsave(struct terminal *term, unsigned param)
{
    switch (param) {
    case 1: term->xtsave.application_cursor_keys = term->cursor_keys_mode == CURSOR_KEYS_APPLICATION; break;
    case 3: break;
    case 4: break;
    case 5: term->xtsave.reverse = term->reverse; break;
    case 6: term->xtsave.origin = term->origin; break;
    case 7: term->xtsave.auto_margin = term->auto_margin; break;
    case 9: /* term->xtsave.mouse_x10 = term->mouse_tracking == MOUSE_X10; */ break;
    case 12: break;
    case 25: term->xtsave.show_cursor = !term->hide_cursor; break;
    case 1000: term->xtsave.mouse_click = term->mouse_tracking == MOUSE_CLICK; break;
    case 1001: break;
    case 1002: term->xtsave.mouse_drag = term->mouse_tracking == MOUSE_DRAG; break;
    case 1003: term->xtsave.mouse_motion = term->mouse_tracking == MOUSE_MOTION; break;
    case 1004: term->xtsave.focus_events = term->focus_events; break;
    case 1005: /* term->xtsave.mouse_utf8 = term->mouse_reporting == MOUSE_UTF8; */ break;
    case 1006: term->xtsave.mouse_sgr = term->mouse_reporting == MOUSE_SGR; break;
    case 1007: term->xtsave.alt_scrolling = term->alt_scrolling; break;
    case 1015: term->xtsave.mouse_urxvt = term->mouse_reporting == MOUSE_URXVT; break;
    case 1034: term->xtsave.meta_eight_bit = term->meta.eight_bit; break;
    case 1036: term->xtsave.meta_esc_prefix = term->meta.esc_prefix; break;
    case 1049: term->xtsave.alt_screen = term->grid == &term->alt; break;
    case 2004: term->xtsave.bracketed_paste = term->bracketed_paste; break;
    }
}

static void
xtrestore(struct terminal *term, unsigned param)
{
    bool enable;
    switch (param) {
    case 1: enable = term->xtsave.application_cursor_keys;
    case 3: return;
    case 4: return;
    case 5: enable = term->xtsave.reverse; break;
    case 6: enable = term->xtsave.origin; break;
    case 7: enable = term->xtsave.auto_margin; break;
    case 9: /* enable = term->xtsave.mouse_x10; break; */ return;
    case 12: return;
    case 25: enable = term->xtsave.show_cursor; break;
    case 1000: enable = term->xtsave.mouse_click; break;
    case 1001: return;
    case 1002: enable = term->xtsave.mouse_drag; break;
    case 1003: enable = term->xtsave.mouse_motion; break;
    case 1004: enable = term->xtsave.focus_events; break;
    case 1005: /* enable = term->xtsave.mouse_utf8; break; */ return;
    case 1006: enable = term->xtsave.mouse_sgr; break;
    case 1007: enable = term->xtsave.alt_scrolling; break;
    case 1015: enable = term->xtsave.mouse_urxvt; break;
    case 1034: enable = term->xtsave.meta_eight_bit; break;
    case 1036: enable = term->xtsave.meta_esc_prefix; break;
    case 1049: enable = term->xtsave.alt_screen; break;
    case 2004: enable = term->xtsave.bracketed_paste; break;
    }

    decset_decrst(term, param, enable);
}

void
csi_dispatch(struct terminal *term, uint8_t final)
{
    LOG_DBG("%s", csi_as_string(term, final, -1));

    switch (term->vt.private[0]) {
    case 0: {
        switch (final) {
        case 'b':
            if (term->vt.last_printed != 0) {
                /*
                 * Note: we never reset 'last-printed'. According to
                 * ECMA-48, the behaviour is undefined if REP was
                 * _not_ preceded by a graphical character.
                 */
                int count = vt_param_get(term, 0, 1);
                LOG_DBG("REP: '%lc' %d times", (wint_t)term->vt.last_printed, count);

                const int width = wcwidth(term->vt.last_printed);
                if (width > 0) {
                    for (int i = 0; i < count; i++)
                        term_print(term, term->vt.last_printed, width);
                }
            }
            break;

        case 'c': {
            /* Send Device Attributes (Primary DA) */

            /*
             * Responses:
             *  - CSI?1;2c      vt100 with advanced video option
             *  - CSI?1;0c      vt101 with no options
             *  - CSI?6c        vt102
             *  - CSI?62;<Ps>c  vt220
             *  - CSI?63;<Ps>c  vt320
             *  - CSI?64;<Ps>c  vt420
             *
             * Ps (response may contain multiple):
             *  - 1    132 columns
             *  - 2    Printer.
             *  - 3    ReGIS graphics.
             *  - 4    Sixel graphics.
             *  - 6    Selective erase.
             *  - 8    User-defined keys.
             *  - 9    National Replacement Character sets.
             *  - 15   Technical characters.
             *  - 16   Locator port.
             *  - 17   Terminal state interrogation.
             *  - 18   User windows.
             *  - 21   Horizontal scrolling.
             *  - 22   ANSI color, e.g., VT525.
             *  - 28   Rectangular editing.
             *  - 29   ANSI text locator (i.e., DEC Locator mode).
             *
             * Note: we report ourselves as a VT220, mainly to be able
             * to pass parameters, to indicate we support sixel, and
             * ANSI colors.
             *
             * The VT level must be synchronized with the secondary DA
             * response.
             *
             * Note: tertiary DA responds with "FOOT".
             */
            const char *reply = "\033[?62;4;22c";
            term_to_slave(term, reply, strlen(reply));
            break;
        }

        case 'd': {
            /* VPA - vertical line position absolute */
            int rel_row = vt_param_get(term, 0, 1) - 1;
            int row = term_row_rel_to_abs(term, rel_row);
            term_cursor_to(term, row, term->grid->cursor.point.col);
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

        case 'a':
        case 'C':
            term_cursor_right(term, vt_param_get(term, 0, 1));
            break;

        case 'D':
            term_cursor_left(term, vt_param_get(term, 0, 1));
            break;

        case 'E':
            /* CNL - Cursor Next Line */
            term_cursor_down(term, vt_param_get(term, 0, 1));
            term_cursor_left(term, term->grid->cursor.point.col);
            break;

        case 'F':
            /* CPL - Cursor Previous Line */
            term_cursor_up(term, vt_param_get(term, 0, 1));
            term_cursor_left(term, term->grid->cursor.point.col);
            break;

        case 'g': {
            int param = vt_param_get(term, 0, 0);
            switch (param) {
            case 0:
                /* Clear tab stop at *current* column */
                tll_foreach(term->tab_stops, it) {
                    if (it->item == term->grid->cursor.point.col)
                        tll_remove(term->tab_stops, it);
                    else if (it->item > term->grid->cursor.point.col)
                        break;
                }

                break;

            case 3:
                /* Clear *all* tabs */
                tll_free(term->tab_stops);
                break;

            default:
                UNHANDLED();
                break;
            }
            break;
        }

        case '`':
        case 'G': {
            /* Cursor horizontal absolute */
            int col = min(vt_param_get(term, 0, 1), term->cols) - 1;
            term_cursor_to(term, term->grid->cursor.point.row, col);
            break;
        }

        case 'f':
        case 'H': {
            /* Move cursor */
            int rel_row = vt_param_get(term, 0, 1) - 1;
            int row = term_row_rel_to_abs(term, rel_row);
            int col = min(vt_param_get(term, 1, 1), term->cols) - 1;
            term_cursor_to(term, row, col);
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
                    &term->grid->cursor.point,
                    &(struct coord){term->cols - 1, term->rows - 1});
                term->grid->cursor.lcf = false;
                break;

            case 1:
                /* From start of screen to cursor */
                term_erase(term, &(struct coord){0, 0}, &term->grid->cursor.point);
                term->grid->cursor.lcf = false;
                break;

            case 2:
                /* Erase entire screen */
                term_erase(
                    term,
                    &(struct coord){0, 0},
                    &(struct coord){term->cols - 1, term->rows - 1});
                term->grid->cursor.lcf = false;
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

                    if (term->render.last_cursor.row == term->grid->rows[i])
                        term->render.last_cursor.row = NULL;

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
                    &term->grid->cursor.point,
                    &(struct coord){term->cols - 1, term->grid->cursor.point.row});
                term->grid->cursor.lcf = false;
                break;

            case 1:
                /* From start of line to cursor */
                term_erase(
                    term, &(struct coord){0, term->grid->cursor.point.row}, &term->grid->cursor.point);
                term->grid->cursor.lcf = false;
                break;

            case 2:
                /* Entire line */
                term_erase(
                    term,
                    &(struct coord){0, term->grid->cursor.point.row},
                    &(struct coord){term->cols - 1, term->grid->cursor.point.row});
                term->grid->cursor.lcf = false;
                break;

            default:
                UNHANDLED();
                break;
            }

            break;
        }

        case 'L': {
            if (term->grid->cursor.point.row < term->scroll_region.start ||
                term->grid->cursor.point.row >= term->scroll_region.end)
                break;

            int count = min(
                vt_param_get(term, 0, 1),
                term->scroll_region.end - term->grid->cursor.point.row);

            term_scroll_reverse_partial(
                term,
                (struct scroll_region){
                    .start = term->grid->cursor.point.row,
                    .end = term->scroll_region.end},
                count);
            term->grid->cursor.lcf = false;
            break;
        }

        case 'M': {
            if (term->grid->cursor.point.row < term->scroll_region.start ||
                term->grid->cursor.point.row >= term->scroll_region.end)
                break;

            int count = min(
                vt_param_get(term, 0, 1),
                term->scroll_region.end - term->grid->cursor.point.row);

            term_scroll_partial(
                term,
                (struct scroll_region){
                    .start = term->grid->cursor.point.row,
                    .end = term->scroll_region.end},
                count);
            term->grid->cursor.lcf = false;
            break;
        }

        case 'P': {
            /* DCH: Delete character(s) */

            /* Number of characters to delete */
            int count = min(
                vt_param_get(term, 0, 1), term->cols - term->grid->cursor.point.col);

            /* Number of characters left after deletion (on current line) */
            int remaining = term->cols - (term->grid->cursor.point.col + count);

            /* 'Delete' characters by moving the remaining ones */
            memmove(&term->grid->cur_row->cells[term->grid->cursor.point.col],
                    &term->grid->cur_row->cells[term->grid->cursor.point.col + count],
                    remaining * sizeof(term->grid->cur_row->cells[0]));

            for (size_t c = 0; c < remaining; c++)
                term->grid->cur_row->cells[term->grid->cursor.point.col + c].attrs.clean = 0;
            term->grid->cur_row->dirty = true;

            /* Erase the remainder of the line */
            term_erase(
                term,
                &(struct coord){term->grid->cursor.point.col + remaining, term->grid->cursor.point.row},
                &(struct coord){term->cols - 1, term->grid->cursor.point.row});
            term->grid->cursor.lcf = false;
            break;
        }

        case '@': {
            /* ICH: insert character(s) */

            /* Number of characters to insert */
            int count = min(
                vt_param_get(term, 0, 1), term->cols - term->grid->cursor.point.col);

            /* Characters to move */
            int remaining = term->cols - (term->grid->cursor.point.col + count);

            /* Push existing characters */
            memmove(&term->grid->cur_row->cells[term->grid->cursor.point.col + count],
                    &term->grid->cur_row->cells[term->grid->cursor.point.col],
                    remaining * sizeof(term->grid->cur_row->cells[0]));
            for (size_t c = 0; c < remaining; c++)
                term->grid->cur_row->cells[term->grid->cursor.point.col + count + c].attrs.clean = 0;
            term->grid->cur_row->dirty = true;

            /* Erase (insert space characters) */
            term_erase(
                term,
                &term->grid->cursor.point,
                &(struct coord){term->grid->cursor.point.col + count - 1, term->grid->cursor.point.row});
            term->grid->cursor.lcf = false;
            break;
        }

        case 'S': {
            int amount = min(
                vt_param_get(term, 0, 1),
                term->scroll_region.end - term->scroll_region.start);
            term_scroll(term, amount);
            break;
        }

        case 'T': {
            int amount = min(
                vt_param_get(term, 0, 1),
                term->scroll_region.end - term->scroll_region.start);
            term_scroll_reverse(term, amount);
            break;
        }

        case 'X': {
            /* Erase chars */
            int count = min(
                vt_param_get(term, 0, 1), term->cols - term->grid->cursor.point.col);

            term_erase(
                term,
                &term->grid->cursor.point,
                &(struct coord){term->grid->cursor.point.col + count - 1, term->grid->cursor.point.row});
            term->grid->cursor.lcf = false;
            break;
        }

        case 'I': {
            /* CHT - Tab Forward (param is number of tab stops to move through) */
            for (int i = 0; i < vt_param_get(term, 0, 1); i++) {
                int new_col = term->cols - 1;
                tll_foreach(term->tab_stops, it) {
                    if (it->item > term->grid->cursor.point.col) {
                        new_col = it->item;
                        break;
                    }
                }
                assert(new_col >= term->grid->cursor.point.col);
                term_cursor_right(term, new_col - term->grid->cursor.point.col);
            }
            break;
        }

        case 'Z':
            /* CBT - Back tab (param is number of tab stops to move back through) */
            for (int i = 0; i < vt_param_get(term, 0, 1); i++) {
                int new_col = 0;
                tll_rforeach(term->tab_stops, it) {
                    if (it->item < term->grid->cursor.point.col) {
                        new_col = it->item;
                        break;
                    }
                }
                assert(term->grid->cursor.point.col >= new_col);
                term_cursor_left(term, term->grid->cursor.point.col - new_col);
            }
            break;

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
                /* TODO: would be easy to implemented; when active
                 * term_linefeed() would _also_ do a
                 * term_carriage_return() */
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
                term_cursor_home(term);

                LOG_DBG("scroll region: %d-%d",
                        term->scroll_region.start,
                        term->scroll_region.end);
            }
            break;
        }

        case 's':
            term->grid->saved_cursor = term->grid->cursor;
            break;

        case 'u':
            term_restore_cursor(term, &term->grid->saved_cursor);
            break;

        case 't': {
            /*
             * Window operations
             */

            const unsigned param = vt_param_get(term, 0, 0);

            switch (param) {
            case 1: LOG_WARN("unimplemented: de-iconify"); break;
            case 2: LOG_WARN("unimplemented: iconify"); break;
            case 3: LOG_WARN("unimplemented: move window to pixel position"); break;
            case 4: LOG_WARN("unimplemented: resize window in pixels"); break;
            case 5: LOG_WARN("unimplemented: raise window to front of stack"); break;
            case 6: LOG_WARN("unimplemented: raise window to back of stack"); break;
            case 7: LOG_WARN("unimplemented: refresh window"); break;
            case 8: LOG_WARN("unimplemented: resize window in chars"); break;
            case 9: LOG_WARN("unimplemented: maximize/unmaximize window"); break;
            case 10: LOG_WARN("unimplemented: to/from full screen"); break;
            case 20: LOG_WARN("unimplemented: report icon label"); break;
            case 21: LOG_WARN("unimplemented: report window title"); break;
            case 24: LOG_WARN("unimplemented: resize window (DECSLPP)"); break;

            case 11:   /* report if window is iconified */
                /* We don't know - always report *not* iconified */
                /* 1=not iconified, 2=iconified */
                term_to_slave(term, "\033[1t", 4);
                break;

            case 13: { /* report window position */

                /* We don't know our position - always report (0,0) */
                int x = -1;
                int y = -1;

                switch (vt_param_get(term, 1, 0)) {
                case 0:
                    /* window position */
                    x = y = 0;
                    break;

                case 2:
                    /* text area position */
                    x = term->margins.left;
                    y = term->margins.top;
                    break;

                default:
                    UNHANDLED();
                    break;
                }

                if (x >= 0 && y >= 0) {
                    char reply[64];
                    snprintf(reply, sizeof(reply), "\033[3;%d;%dt",
                             x / term->scale, y / term->scale);
                    term_to_slave(term, reply, strlen(reply));
                }
                break;
            }

            case 14: { /* report window size in pixels */
                int width = -1;
                int height = -1;

                switch (vt_param_get(term, 1, 0)) {
                case 0:
                    /* text area size */
                    width = term->width - term->margins.left - term->margins.right;
                    height = term->height - term->margins.top - term->margins.bottom;
                    break;

                case 2:
                    /* window size */
                    width = term->width;
                    height = term->height;
                    break;

                default:
                    UNHANDLED();
                    break;
                }

                if (width >= 0 && height >= 0) {
                    char reply[64];
                    snprintf(reply, sizeof(reply), "\033[4;%d;%dt",
                             height / term->scale, width / term->scale);
                    term_to_slave(term, reply, strlen(reply));
                }
                break;
            }

            case 15:   /* report screen size in pixels */
                tll_foreach(term->window->on_outputs, it) {
                    char reply[64];
                    snprintf(reply, sizeof(reply), "\033[5;%d;%dt",
                             it->item->dim.px_scaled.height,
                             it->item->dim.px_scaled.width);
                    term_to_slave(term, reply, strlen(reply));
                    break;
                }

                if (tll_length(term->window->on_outputs) == 0)
                    term_to_slave(term, "\033[5;0;0t", 8);
                break;

            case 16: { /* report cell size in pixels */
                char reply[64];
                snprintf(reply, sizeof(reply), "\033[6;%d;%dt",
                         term->cell_height / term->scale,
                         term->cell_width / term->scale);
                term_to_slave(term, reply, strlen(reply));
                break;
            }

            case 18: { /* text area size in chars */
                char reply[64];
                snprintf(reply, sizeof(reply), "\033[8;%d;%dt",
                         term->rows, term->cols);
                term_to_slave(term, reply, strlen(reply));
                break;
            }

            case 19: { /* report screen size in chars */
                tll_foreach(term->window->on_outputs, it) {
                    char reply[64];
                    snprintf(reply, sizeof(reply), "\033[9;%d;%dt",
                             it->item->dim.px_real.height / term->cell_height / term->scale,
                             it->item->dim.px_real.width / term->cell_width / term->scale);
                    term_to_slave(term, reply, strlen(reply));
                    break;
                }

                if (tll_length(term->window->on_outputs) == 0)
                    term_to_slave(term, "\033[9;0;0t", 8);
                break;
            }

            case 22: { /* push window title */
                /* 0 - icon + title, 1 - icon, 2 - title */
                unsigned what = vt_param_get(term, 1, 0);
                if (what == 0 || what == 2) {
                    tll_push_back(
                        term->window_title_stack, xstrdup(term->window_title));
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
                LOG_DBG("ignoring %s", csi_as_string(term, final, -1));
                break;
            }
            break;
        }

        case 'n': {
            if (term->vt.params.idx > 0) {
                int param = vt_param_get(term, 0, 0);
                switch (param) {
                case 5:
                    /* Query device status */
                    term_to_slave(term, "\x1b[0n", 4);  /* "Device OK" */
                    break;

                case 6: {
                    /* u7 - cursor position query */

                    int row = term->origin == ORIGIN_ABSOLUTE
                        ? term->grid->cursor.point.row
                        : term->grid->cursor.point.row - term->scroll_region.start;

                    /* TODO: we use 0-based position, while the xterm
                     * terminfo says the receiver of the reply should
                     * decrement, hence we must add 1 */
                    char reply[64];
                    snprintf(reply, sizeof(reply), "\x1b[%d;%dR",
                             row + 1, term->grid->cursor.point.col + 1);
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
        case 'h':
            /* DECSET - DEC private mode set */
            for (size_t i = 0; i < term->vt.params.idx; i++)
                decset(term, term->vt.params.v[i].value);
            break;

        case 'l':
            /* DECRST - DEC private mode reset */
            for (size_t i = 0; i < term->vt.params.idx; i++)
                decrst(term, term->vt.params.v[i].value);
            break;

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
            for (size_t i = 0; i < term->vt.params.idx; i++)
                xtsave(term, term->vt.params.v[i].value);
            break;

        case 'r':
            for (size_t i = 0; i < term->vt.params.idx; i++)
                xtrestore(term, term->vt.params.v[i].value);
            break;

        case 'S': {
            unsigned target = vt_param_get(term, 0, 0);
            unsigned operation = vt_param_get(term, 1, 0);

            switch (target) {
            case 1:
                switch (operation) {
                case 1: sixel_colors_report_current(term); break;
                case 2: sixel_colors_reset(term); break;
                case 3: sixel_colors_set(term, vt_param_get(term, 2, 0)); break;
                case 4: sixel_colors_report_max(term);
                default: UNHANDLED(); break;
                }
                break;

            case 2:
                switch (operation) {
                case 1: sixel_geometry_report_current(term); break;
                case 2: sixel_geometry_reset(term); break;
                case 3: sixel_geometry_set(term, vt_param_get(term, 2, 0), vt_param_get(term, 3, 0)); break;
                case 4: sixel_geometry_report_max(term);
                default: UNHANDLED(); break;
                }
                break;

            default:
                UNHANDLED();
                break;
            }

            break;
        }

        default:
            UNHANDLED();
            break;
        }

        break; /* private == '?' */
    }

    case '>': {
        switch (final) {
            case 'c':
                /* Send Device Attributes (Secondary DA) */
                if (vt_param_get(term, 0, 0) != 0) {
                    UNHANDLED();
                    break;
                }

                /*
                 * Param 1 - terminal type:
                 *   0 - vt100
                 *   1 - vt220
                 *   2 - vt240
                 *  18 - vt330
                 *  19 - vt340
                 *  24 - vt320
                 *  41 - vt420
                 *  61 - vt510
                 *  64 - vt520
                 *  65 - vt525
                 *
                 * Param 2 - firmware version
                 *  xterm uses its version number. We use an xterm
                 *  version number too, since e.g. Emacs uses this to
                 *  determine level of support.
                 *
                 * We report ourselves as a VT220. This must be
                 * synchronized with the primary DA response.
                 *
                 * Note: tertiary DA replies with "FOOT".
                 */

                static_assert(FOOT_MAJOR < 100, "Major version must not exceed 99");
                static_assert(FOOT_MINOR < 100, "Minor version must not exceed 99");
                static_assert(FOOT_PATCH < 100, "Patch version must not exceed 99");

                char reply[64];
                snprintf(reply, sizeof(reply), "\033[>1;%02u%02u%02u;0c",
                         FOOT_MAJOR, FOOT_MINOR, FOOT_PATCH);

                term_to_slave(term, reply, strlen(reply));
                break;

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
                             resource, csi_as_string(term, final, -1));
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
            case 0: /* blinking block, but we use it to reset to configured default */
                term->cursor_style = term->default_cursor_style;
                if (term->default_cursor_blink)
                    term_cursor_blink_enable(term);
                else
                    term_cursor_blink_disable(term);
                break;

            case 1:         /* blinking block */
            case 2:         /* steady block */
                term->cursor_style = CURSOR_BLOCK;
                break;

            case 3:         /* blinking underline */
            case 4:         /* steady underline */
                term->cursor_style = CURSOR_UNDERLINE;
                break;

            case 5:         /* blinking bar */
            case 6:         /* steady bar */
                term->cursor_style = CURSOR_BAR;
                break;

            default:
                UNHANDLED();
                break;
            }

            if (param > 0 && param <= 6) {
                if (param & 1)
                    term_cursor_blink_enable(term);
                else
                    term_cursor_blink_disable(term);
            }
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

    case '=': {
        switch (final) {
        case 'c':
            if (vt_param_get(term, 0, 0) != 0) {
                UNHANDLED();
                break;
            }

            /*
             * Send Device Attributes (Tertiary DA)
             *
             * Reply format is "DCS ! | DDDDDDDD ST"
             *
             * D..D is the unit ID of the terminal, consisting of four
             * hexadecimal pairs. The first pair represents the
             * manufacturing site code. This code can be any
             * hexadecimal value from 00 through FF.
             */

            term_to_slave(term, "\033P!|464f4f54\033\\", 14);  /* FOOT */
            break;

        default:
            UNHANDLED();
            break;
        }
        break; /* private == '=' */
    }

    default:
        UNHANDLED();
        break;
    }
}
