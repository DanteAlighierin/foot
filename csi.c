#include "csi.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(_DEBUG)
 #include <stdio.h>
#endif

#include <sys/timerfd.h>

#define LOG_MODULE "csi"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "config.h"
#include "debug.h"
#include "grid.h"
#include "selection.h"
#include "sixel.h"
#include "util.h"
#include "version.h"
#include "vt.h"
#include "xmalloc.h"
#include "xsnprintf.h"

#define UNHANDLED()        LOG_DBG("unhandled: %s", csi_as_string(term, final, -1))
#define UNHANDLED_SGR(idx) LOG_DBG("unhandled: %s", csi_as_string(term, 'm', idx))

static void
sgr_reset(struct terminal *term)
{
    term->vt.attrs = (struct attributes){0};
    term->vt.underline = (struct underline_range_data){0};

    term->bits_affecting_ascii_printer.underline_style = false;
    term->bits_affecting_ascii_printer.underline_color = false;
    term_update_ascii_printer(term);
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

    for (size_t i = 0; i < sizeof(term->vt.private); i++) {
        char value = (term->vt.private >> (i * 8)) & 0xff;
        if (value == 0)
            break;
        c += snprintf(&msg[c], sizeof(msg) - c, "%c", value);
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
        case 4: {
            term->vt.attrs.underline = true;
            term->vt.underline.style = UNDERLINE_SINGLE;

            if (unlikely(term->vt.params.v[i].sub.idx == 1)) {
                enum underline_style style = term->vt.params.v[i].sub.value[0];

                switch (style) {
                default:
                case UNDERLINE_NONE:
                    term->vt.attrs.underline = false;
                    term->vt.underline.style = UNDERLINE_NONE;
                    term->bits_affecting_ascii_printer.underline_style = false;
                    break;

                case UNDERLINE_SINGLE:
                case UNDERLINE_DOUBLE:
                case UNDERLINE_CURLY:
                case UNDERLINE_DOTTED:
                case UNDERLINE_DASHED:
                    term->vt.underline.style = style;
                    term->bits_affecting_ascii_printer.underline_style =
                        style > UNDERLINE_SINGLE;
                    break;
                }

                term_update_ascii_printer(term);
            }
            break;
        }
        case 5: term->vt.attrs.blink = true; break;
        case 6: LOG_WARN("ignored: rapid blink"); break;
        case 7: term->vt.attrs.reverse = true; break;
        case 8: term->vt.attrs.conceal = true; break;
        case 9: term->vt.attrs.strikethrough = true; break;

        case 21:
            term->vt.attrs.underline = true;
            term->vt.underline.style = UNDERLINE_DOUBLE;
            term->bits_affecting_ascii_printer.underline_style = true;
            term_update_ascii_printer(term);
            break;

        case 22: term->vt.attrs.bold = term->vt.attrs.dim = false; break;
        case 23: term->vt.attrs.italic = false; break;
        case 24: {
            term->vt.attrs.underline = false;
            term->vt.underline.style = UNDERLINE_NONE;
            term->bits_affecting_ascii_printer.underline_style = false;
            term_update_ascii_printer(term);
            break;
        }
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
            term->vt.attrs.fg_src = COLOR_BASE16;
            term->vt.attrs.fg = param - 30;
            break;

        case 38:
        case 48:
        case 58: {
            uint32_t color;
            enum color_source src;

            /* Indexed: 38;5;<idx> */
            if (term->vt.params.idx - i - 1 >= 2 &&
                term->vt.params.v[i + 1].value == 5)
            {
                src = COLOR_BASE256;
                color = min(term->vt.params.v[i + 2].value,
                            ALEN(term->colors.table) - 1);
                i += 2;
            }

            /* RGB: 38;2;<r>;<g>;<b> */
            else if (term->vt.params.idx - i - 1 >= 4 &&
                     term->vt.params.v[i + 1].value == 2)
            {
                uint8_t r = term->vt.params.v[i + 2].value;
                uint8_t g = term->vt.params.v[i + 3].value;
                uint8_t b = term->vt.params.v[i + 4].value;
                src = COLOR_RGB;
                color = r << 16 | g << 8 | b;
                i += 4;
            }

            /* Indexed: 38:5:<idx> */
            else if (term->vt.params.v[i].sub.idx >= 2 &&
                     term->vt.params.v[i].sub.value[0] == 5)
            {
                src = COLOR_BASE256;
                color = min(term->vt.params.v[i].sub.value[1],
                            ALEN(term->colors.table) - 1);
            }

            /*
             * RGB: 38:2:<color-space>:r:g:b[:ignored:tolerance:tolerance-color-space]
             * RGB: 38:2:r:g:b
             *
             * The second version is a "bastard" version - many
             * programs "forget" the color space ID
             * parameter... *sigh*
             */
            else if (term->vt.params.v[i].sub.idx >= 4 &&
                     term->vt.params.v[i].sub.value[0] == 2)
            {
                const struct vt_param *param = &term->vt.params.v[i];
                bool have_color_space_id = param->sub.idx >= 5;

                /* 0 - color space (ignored) */
                int r_idx = 2 - !have_color_space_id;
                int g_idx = 3 - !have_color_space_id;
                int b_idx = 4 - !have_color_space_id;
                /* 5 - unused */
                /* 6 - CS tolerance */
                /* 7 - color space associated with tolerance */

                uint8_t r = param->sub.value[r_idx];
                uint8_t g = param->sub.value[g_idx];
                uint8_t b = param->sub.value[b_idx];

                src = COLOR_RGB;
                color = r << 16 | g << 8 | b;
            }

            /* Transparent: 38:1 */
            /* CMY:         38:3:<color-space>:c:m:y[:tolerance:tolerance-color-space] */
            /* CMYK:        38:4:<color-space>:c:m:y:k[:tolerance:tolerance-color-space] */

            /* Unrecognized */
            else {
                UNHANDLED_SGR(i);
                break;
            }

            if (unlikely(param == 58)) {
                term->vt.underline.color_src = src;
                term->vt.underline.color = color;
                term->bits_affecting_ascii_printer.underline_color = true;
                term_update_ascii_printer(term);
            } else if (param == 38) {
                term->vt.attrs.fg_src = src;
                term->vt.attrs.fg = color;
            } else {
                xassert(param == 48);
                term->vt.attrs.bg_src = src;
                term->vt.attrs.bg = color;
            }
            break;
        }

        case 39:
            term->vt.attrs.fg_src = COLOR_DEFAULT;
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
            term->vt.attrs.bg_src = COLOR_BASE16;
            term->vt.attrs.bg = param - 40;
            break;

        case 49:
            term->vt.attrs.bg_src = COLOR_DEFAULT;
            break;

        case 59:
            term->vt.underline.color_src = COLOR_DEFAULT;
            term->vt.underline.color = 0;
            term->bits_affecting_ascii_printer.underline_color = false;
            term_update_ascii_printer(term);
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
            term->vt.attrs.fg_src = COLOR_BASE16;
            term->vt.attrs.fg = param - 90 + 8;
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
            term->vt.attrs.bg_src = COLOR_BASE16;
            term->vt.attrs.bg = param - 100 + 8;
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

    case 5:
        /* DECSCNM */
        term->reverse = enable;
        term_damage_all(term);
        term_damage_margins(term);
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
        term->cursor_blink.decset = enable;
        term_cursor_blink_update(term);
        break;

    case 25:
        /* DECTCEM */
        term->hide_cursor = !enable;
        break;

    case 45:
        term->reverse_wrap = enable;
        break;

    case 66:
        /* DECNKM */
        term->keypad_keys_mode = enable ? KEYPAD_APPLICATION : KEYPAD_NUMERICAL;
        break;

    case 67:
        if (enable)
            LOG_WARN("unimplemented: DECBKM");
        break;

    case 80:
        term->sixel.scrolling = !enable;
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

    case 1016:
        if (enable)
            term->mouse_reporting = MOUSE_SGR_PIXELS;
        else if (term->mouse_reporting == MOUSE_SGR_PIXELS)
            term->mouse_reporting = MOUSE_NORMAL;
        break;

    case 1034:
        /* smm */
        LOG_DBG("%s 8-bit meta mode", enable ? "enabling" : "disabling");
        term->meta.eight_bit = enable;
        break;

    case 1035:
        /* numLock */
        LOG_DBG("%s Num Lock modifier", enable ? "enabling" : "disabling");
        term->num_lock_modifier = enable;
        break;

    case 1036:
        /* metaSendsEscape */
        LOG_DBG("%s meta-sends-escape", enable ? "enabling" : "disabling");
        term->meta.esc_prefix = enable;
        break;

    case 1042:
        term->bell_action_enabled = enable;
        break;

#if 0
    case 1043:
        LOG_WARN("unimplemented: raise window on ctrl-g");
        break;
#endif

    case 1048:
        if (enable)
            term_save_cursor(term);
        else
            term_restore_cursor(term, &term->grid->saved_cursor);
        break;

    case 47:
    case 1047:
    case 1049:
        if (enable && term->grid != &term->alt) {
            selection_cancel(term);

            if (param == 1049)
                term_save_cursor(term);

            term->grid = &term->alt;

            /* Cursor retains its position from the normal grid */
            term_cursor_to(
                term,
                min(term->normal.cursor.point.row, term->rows - 1),
                min(term->normal.cursor.point.col, term->cols - 1));

            tll_free(term->normal.scroll_damage);
            term_erase(term, 0, 0, term->rows - 1, term->cols - 1);
        }

        else if (!enable && term->grid == &term->alt) {
            selection_cancel(term);

            term->grid = &term->normal;

            /* Cursor retains its position from the alt grid */
            term_cursor_to(
                term, min(term->alt.cursor.point.row, term->rows - 1),
                min(term->alt.cursor.point.col, term->cols - 1));

            if (param == 1049)
                term_restore_cursor(term, &term->grid->saved_cursor);

            /* Delete all sixel images on the alt screen */
            tll_foreach(term->alt.sixel_images, it) {
                sixel_destroy(&it->item);
                tll_remove(term->alt.sixel_images, it);
            }

            tll_free(term->alt.scroll_damage);
            term_damage_view(term);
        }

        term->bits_affecting_ascii_printer.sixels =
            tll_length(term->grid->sixel_images) > 0;
        term_update_ascii_printer(term);
        break;

    case 1070:
        term->sixel.use_private_palette = enable;
        break;

    case 2004:
        term->bracketed_paste = enable;
        break;

    case 2026:
        if (enable)
            term_enable_app_sync_updates(term);
        else
            term_disable_app_sync_updates(term);
        break;

    case 2027:
        term->grapheme_shaping = enable;
        break;

    case 2048:
        if (enable)
            term_enable_size_notifications(term);
        else
            term_disable_size_notifications(term);
        break;

    case 8452:
        term->sixel.cursor_right_of_graphics = enable;
        break;

    case 737769:
        if (enable)
            term_ime_enable(term);
        else {
            term_ime_disable(term);
            term->ime_reenable_after_url_mode = false;
        }
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

/*
 * These values represent the current state of a DEC private mode,
 * as returned in the DECRPM reply to a DECRQM query.
 */
enum decrpm_status {
    DECRPM_NOT_RECOGNIZED = 0,
    DECRPM_SET = 1,
    DECRPM_RESET = 2,
    DECRPM_PERMANENTLY_SET = 3,
    DECRPM_PERMANENTLY_RESET = 4,
};

static enum decrpm_status
decrpm(bool enabled)
{
    return enabled ? DECRPM_SET : DECRPM_RESET;
}

static enum decrpm_status
decrqm(const struct terminal *term, unsigned param)
{
    switch (param) {
    case 1: return decrpm(term->cursor_keys_mode == CURSOR_KEYS_APPLICATION);
    case 5: return decrpm(term->reverse);
    case 6: return decrpm(term->origin);
    case 7: return decrpm(term->auto_margin);
    case 9: return DECRPM_PERMANENTLY_RESET; /* term->mouse_tracking == MOUSE_X10; */
    case 12: return decrpm(term->cursor_blink.decset);
    case 25: return decrpm(!term->hide_cursor);
    case 45: return decrpm(term->reverse_wrap);
    case 66: return decrpm(term->keypad_keys_mode == KEYPAD_APPLICATION);
    case 67: return DECRPM_PERMANENTLY_RESET; /* https://vt100.net/docs/vt510-rm/DECBKM */
    case 80: return decrpm(!term->sixel.scrolling);
    case 1000: return decrpm(term->mouse_tracking == MOUSE_CLICK);
    case 1001: return DECRPM_PERMANENTLY_RESET;
    case 1002: return decrpm(term->mouse_tracking == MOUSE_DRAG);
    case 1003: return decrpm(term->mouse_tracking == MOUSE_MOTION);
    case 1004: return decrpm(term->focus_events);
    case 1005: return DECRPM_PERMANENTLY_RESET; /* term->mouse_reporting == MOUSE_UTF8; */
    case 1006: return decrpm(term->mouse_reporting == MOUSE_SGR);
    case 1007: return decrpm(term->alt_scrolling);
    case 1015: return decrpm(term->mouse_reporting == MOUSE_URXVT);
    case 1016: return decrpm(term->mouse_reporting == MOUSE_SGR_PIXELS);
    case 1034: return decrpm(term->meta.eight_bit);
    case 1035: return decrpm(term->num_lock_modifier);
    case 1036: return decrpm(term->meta.esc_prefix);
    case 1042: return decrpm(term->bell_action_enabled);
    case 47:   /* FALLTHROUGH */
    case 1047: /* FALLTHROUGH */
    case 1049: return decrpm(term->grid == &term->alt);
    case 1070: return decrpm(term->sixel.use_private_palette);
    case 2004: return decrpm(term->bracketed_paste);
    case 2026: return decrpm(term->render.app_sync_updates.enabled);
    case 2027: return term->conf->tweak.grapheme_width_method != GRAPHEME_WIDTH_DOUBLE
        ? DECRPM_PERMANENTLY_RESET
        : decrpm(term->grapheme_shaping);
    case 2048: return decrpm(term->size_notifications);
    case 8452: return decrpm(term->sixel.cursor_right_of_graphics);
    case 737769: return decrpm(term_ime_is_enabled(term));
    }

    return DECRPM_NOT_RECOGNIZED;
}

static void
xtsave(struct terminal *term, unsigned param)
{
    switch (param) {
    case 1: term->xtsave.application_cursor_keys = term->cursor_keys_mode == CURSOR_KEYS_APPLICATION; break;
    case 5: term->xtsave.reverse = term->reverse; break;
    case 6: term->xtsave.origin = term->origin; break;
    case 7: term->xtsave.auto_margin = term->auto_margin; break;
    case 9: /* term->xtsave.mouse_x10 = term->mouse_tracking == MOUSE_X10; */ break;
    case 12: term->xtsave.cursor_blink = term->cursor_blink.decset; break;
    case 25: term->xtsave.show_cursor = !term->hide_cursor; break;
    case 45: term->xtsave.reverse_wrap = term->reverse_wrap; break;
    case 47: term->xtsave.alt_screen = term->grid == &term->alt; break;
    case 66: term->xtsave.application_keypad_keys = term->keypad_keys_mode == KEYPAD_APPLICATION; break;
    case 67: break;
    case 80: term->xtsave.sixel_display_mode = !term->sixel.scrolling; break;
    case 1000: term->xtsave.mouse_click = term->mouse_tracking == MOUSE_CLICK; break;
    case 1001: break;
    case 1002: term->xtsave.mouse_drag = term->mouse_tracking == MOUSE_DRAG; break;
    case 1003: term->xtsave.mouse_motion = term->mouse_tracking == MOUSE_MOTION; break;
    case 1004: term->xtsave.focus_events = term->focus_events; break;
    case 1005: /* term->xtsave.mouse_utf8 = term->mouse_reporting == MOUSE_UTF8; */ break;
    case 1006: term->xtsave.mouse_sgr = term->mouse_reporting == MOUSE_SGR; break;
    case 1007: term->xtsave.alt_scrolling = term->alt_scrolling; break;
    case 1015: term->xtsave.mouse_urxvt = term->mouse_reporting == MOUSE_URXVT; break;
    case 1016: term->xtsave.mouse_sgr_pixels = term->mouse_reporting == MOUSE_SGR_PIXELS; break;
    case 1034: term->xtsave.meta_eight_bit = term->meta.eight_bit; break;
    case 1035: term->xtsave.num_lock_modifier = term->num_lock_modifier; break;
    case 1036: term->xtsave.meta_esc_prefix = term->meta.esc_prefix; break;
    case 1042: term->xtsave.bell_action_enabled = term->bell_action_enabled; break;
    case 1047: term->xtsave.alt_screen = term->grid == &term->alt; break;
    case 1048: term_save_cursor(term); break;
    case 1049: term->xtsave.alt_screen = term->grid == &term->alt; break;
    case 1070: term->xtsave.sixel_private_palette = term->sixel.use_private_palette; break;
    case 2004: term->xtsave.bracketed_paste = term->bracketed_paste; break;
    case 2026: term->xtsave.app_sync_updates = term->render.app_sync_updates.enabled; break;
    case 2027: term->xtsave.grapheme_shaping = term->grapheme_shaping; break;
    case 2048: term->xtsave.size_notifications = term->size_notifications; break;
    case 8452: term->xtsave.sixel_cursor_right_of_graphics = term->sixel.cursor_right_of_graphics; break;
    case 737769: term->xtsave.ime = term_ime_is_enabled(term); break;
    }
}

static void
xtrestore(struct terminal *term, unsigned param)
{
    bool enable;
    switch (param) {
    case 1: enable = term->xtsave.application_cursor_keys; break;
    case 5: enable = term->xtsave.reverse; break;
    case 6: enable = term->xtsave.origin; break;
    case 7: enable = term->xtsave.auto_margin; break;
    case 9: /* enable = term->xtsave.mouse_x10; break; */ return;
    case 12: enable = term->xtsave.cursor_blink; break;
    case 25: enable = term->xtsave.show_cursor; break;
    case 45: enable = term->xtsave.reverse_wrap; break;
    case 47: enable = term->xtsave.alt_screen; break;
    case 66: enable = term->xtsave.application_keypad_keys; break;
    case 67: return;
    case 80: enable = term->xtsave.sixel_display_mode; break;
    case 1000: enable = term->xtsave.mouse_click; break;
    case 1001: return;
    case 1002: enable = term->xtsave.mouse_drag; break;
    case 1003: enable = term->xtsave.mouse_motion; break;
    case 1004: enable = term->xtsave.focus_events; break;
    case 1005: /* enable = term->xtsave.mouse_utf8; break; */ return;
    case 1006: enable = term->xtsave.mouse_sgr; break;
    case 1007: enable = term->xtsave.alt_scrolling; break;
    case 1015: enable = term->xtsave.mouse_urxvt; break;
    case 1016: enable = term->xtsave.mouse_sgr_pixels; break;
    case 1034: enable = term->xtsave.meta_eight_bit; break;
    case 1035: enable = term->xtsave.num_lock_modifier; break;
    case 1036: enable = term->xtsave.meta_esc_prefix; break;
    case 1042: enable = term->xtsave.bell_action_enabled; break;
    case 1047: enable = term->xtsave.alt_screen; break;
    case 1048: enable = true; break;
    case 1049: enable = term->xtsave.alt_screen; break;
    case 1070: enable = term->xtsave.sixel_private_palette; break;
    case 2004: enable = term->xtsave.bracketed_paste; break;
    case 2026: enable = term->xtsave.app_sync_updates; break;
    case 2027: enable = term->xtsave.grapheme_shaping; break;
    case 2048: enable = term->xtsave.size_notifications; break;
    case 8452: enable = term->xtsave.sixel_cursor_right_of_graphics; break;
    case 737769: enable = term->xtsave.ime; break;

    default: return;
    }

    decset_decrst(term, param, enable);
}

static bool
params_to_rectangular_area(const struct terminal *term, int first_idx,
                           int *top, int *left, int *bottom, int *right)
{
    int rel_top = vt_param_get(term, first_idx + 0, 1) - 1;
    *left = min(vt_param_get(term, first_idx + 1, 1) - 1, term->cols - 1);
    int rel_bottom = vt_param_get(term, first_idx + 2, term->rows) - 1;
    *right = min(vt_param_get(term, first_idx + 3, term->cols) - 1, term->cols - 1);

    if (rel_top > rel_bottom || *left > *right)
        return false;

    *top = term_row_rel_to_abs(term, rel_top);
    *bottom = term_row_rel_to_abs(term, rel_bottom);

    return true;
}

void
csi_dispatch(struct terminal *term, uint8_t final)
{
    LOG_DBG("%s (%08x)", csi_as_string(term, final, -1), term->vt.private);

    switch (term->vt.private) {
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

                const int width = c32width(term->vt.last_printed);
                if (width > 0) {
                    for (int i = 0; i < count; i++)
                        term_print(term, term->vt.last_printed, width);
                }
            }
            break;

        case 'c': {
            if (vt_param_get(term, 0, 0) != 0) {
                UNHANDLED();
                break;
            }

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
            if (term->conf->tweak.sixel) {
                static const char reply[] = "\033[?62;4;22;28c";
                term_to_slave(term, reply, sizeof(reply) - 1);
            } else {
                static const char reply[] = "\033[?62;22;28c";
                term_to_slave(term, reply, sizeof(reply) - 1);
            }
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
            term_cursor_col(term, col);
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
            case 0: {
                /* From cursor to end of screen */
                const struct coord *cursor = &term->grid->cursor.point;
                term_erase(
                    term,
                    cursor->row, cursor->col,
                    term->rows - 1, term->cols - 1);
                term->grid->cursor.lcf = false;
                break;
            }

            case 1: {
                /* From start of screen to cursor */
                const struct coord *cursor = &term->grid->cursor.point;
                term_erase(term, 0, 0, cursor->row, cursor->col);
                term->grid->cursor.lcf = false;
                break;
            }

            case 2:
                /* Erase entire screen */
                term_erase(term, 0, 0, term->rows - 1, term->cols - 1);
                term->grid->cursor.lcf = false;
                break;

            case 3: {
                /* Erase scrollback */
                term_erase_scrollback(term);
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
            case 0: {
                /* From cursor to end of line */
                const struct coord *cursor = &term->grid->cursor.point;
                term_erase(
                    term,
                    cursor->row, cursor->col,
                    cursor->row, term->cols - 1);
                term->grid->cursor.lcf = false;
                break;
            }

            case 1: {
                /* From start of line to cursor */
                const struct coord *cursor = &term->grid->cursor.point;
                term_erase(term, cursor->row, 0, cursor->row, cursor->col);
                term->grid->cursor.lcf = false;
                break;
            }

            case 2: {
                /* Entire line */
                const struct coord *cursor = &term->grid->cursor.point;
                term_erase(term, cursor->row, 0, cursor->row, term->cols - 1);
                term->grid->cursor.lcf = false;
                break;
            }

            default:
                UNHANDLED();
                break;
            }

            break;
        }

        case 'L': {  /* IL */
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
            term->grid->cursor.point.col = 0;
            break;
        }

        case 'M': {  /* DL */
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
            term->grid->cursor.point.col = 0;
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
            const struct coord *cursor = &term->grid->cursor.point;
            term_erase(
                term,
                cursor->row, cursor->col + remaining,
                cursor->row, term->cols - 1);
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
            const struct coord *cursor = &term->grid->cursor.point;
            term_erase(
                term,
                cursor->row, cursor->col,
                cursor->row, cursor->col + count - 1);
            term->grid->cursor.lcf = false;
            break;
        }

        case 'S': {
            const struct scroll_region *r = &term->scroll_region;
            int amount = min(vt_param_get(term, 0, 1), r->end - r->start);
            term_scroll(term, amount);
            break;
        }

        case 'T': {
            const struct scroll_region *r = &term->scroll_region;
            int amount = min(vt_param_get(term, 0, 1), r->end - r->start);
            term_scroll_reverse(term, amount);
            break;
        }

        case 'X': {
            /* Erase chars */
            int count = min(
                vt_param_get(term, 0, 1), term->cols - term->grid->cursor.point.col);

            const struct coord *cursor = &term->grid->cursor.point;
            term_erase(
                term,
                cursor->row, cursor->col,
                cursor->row, cursor->col + count - 1);
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
                xassert(new_col >= term->grid->cursor.point.col);

                bool lcf = term->grid->cursor.lcf;
                term_cursor_right(term, new_col - term->grid->cursor.point.col);
                term->grid->cursor.lcf = lcf;
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
                xassert(term->grid->cursor.point.col >= new_col);
                term_cursor_left(term, term->grid->cursor.point.col - new_col);
            }
            break;

        case 'h':
        case 'l': {
            /* Set/Reset Mode (SM/RM) */
            int param = vt_param_get(term, 0, 0);
            bool sm = final == 'h';
            if (param == 4) {
                /* Insertion Replacement Mode (IRM) */
                term->insert_mode = sm;
                term->bits_affecting_ascii_printer.insert_mode = sm;
                term_update_ascii_printer(term);
                break;
            }

            /*
             * ECMA-48 defines modes 1-22, all of which were optional
             * (§7.1; "may have one state only") and are considered
             * deprecated (§7.1) in the latest (5th) edition. xterm only
             * documents modes 2, 4, 12 and 20, the last of which was
             * outright removed (§8.3.106) in 5th edition ECMA-48.
             */
            if (sm) {
                LOG_WARN("SM with unimplemented mode: %d", param);
            }
            break;
        }

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
            term_save_cursor(term);
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
            case 24: LOG_WARN("unimplemented: resize window (DECSLPP)"); break;

            case 11:   /* report if window is iconified */
                /* We don't know - always report *not* iconified */
                /* 1=not iconified, 2=iconified */
                term_to_slave(term, "\033[1t", 4);
                break;

            case 13: { /* report window position */
                /* We don't know our position - always report (0,0) */
                static const char reply[] = "\033[3;0;0t";
                switch (vt_param_get(term, 1, 0)) {
                case 0: /* window position */
                case 2: /* text area position */
                    term_to_slave(term, reply, sizeof(reply) - 1);
                    break;

                default:
                    UNHANDLED();
                    break;
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
                    size_t n = xsnprintf(
                        reply, sizeof(reply), "\033[4;%d;%dt", height, width);
                    term_to_slave(term, reply, n);
                }
                break;
            }

            case 15:   /* report screen size in pixels */
                tll_foreach(term->window->on_outputs, it) {
                    char reply[64];
                    size_t n = xsnprintf(reply, sizeof(reply), "\033[5;%d;%dt",
                             it->item->dim.px_real.height,
                             it->item->dim.px_real.width);
                    term_to_slave(term, reply, n);
                    break;
                }

                if (tll_length(term->window->on_outputs) == 0)
                    term_to_slave(term, "\033[5;0;0t", 8);
                break;

            case 16: { /* report cell size in pixels */
                char reply[64];
                size_t n = xsnprintf(
                    reply, sizeof(reply), "\033[6;%d;%dt",
                    term->cell_height, term->cell_width);
                term_to_slave(term, reply, n);
                break;
            }

            case 18: { /* text area size in chars */
                char reply[64];
                size_t n = xsnprintf(reply, sizeof(reply), "\033[8;%d;%dt",
                         term->rows, term->cols);
                term_to_slave(term, reply, n);
                break;
            }

            case 19: { /* report screen size in chars */
                tll_foreach(term->window->on_outputs, it) {
                    char reply[64];
                    size_t n = xsnprintf(
                        reply, sizeof(reply), "\033[9;%d;%dt",
                        it->item->dim.px_real.height / term->cell_height,
                        it->item->dim.px_real.width / term->cell_width);
                    term_to_slave(term, reply, n);
                    break;
                }

                if (tll_length(term->window->on_outputs) == 0)
                    term_to_slave(term, "\033[9;0;0t", 8);
                break;
            }

            case 21: {
                char reply[3 + strlen(term->window_title) + 2 + 1];
                int chars = xsnprintf(
                    reply, sizeof(reply), "\033]l%s\033\\", term->window_title);
                term_to_slave(term, reply, chars);
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
                    size_t n = xsnprintf(reply, sizeof(reply), "\x1b[%d;%dR",
                             row + 1, term->grid->cursor.point.col + 1);
                    term_to_slave(term, reply, n);
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

        break;  /* private[0] == 0 */
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

        case 's':
            for (size_t i = 0; i < term->vt.params.idx; i++)
                xtsave(term, term->vt.params.v[i].value);
            break;

        case 'r':
            for (size_t i = 0; i < term->vt.params.idx; i++)
                xtrestore(term, term->vt.params.v[i].value);
            break;

        case 'S': {
            if (!term->conf->tweak.sixel) {
                UNHANDLED();
                break;
            }

            unsigned target = vt_param_get(term, 0, 0);
            unsigned operation = vt_param_get(term, 1, 0);

            switch (target) {
            case 1:
                switch (operation) {
                case 1: sixel_colors_report_current(term); break;
                case 2: sixel_colors_reset(term); break;
                case 3: sixel_colors_set(term, vt_param_get(term, 2, 0)); break;
                case 4: sixel_colors_report_max(term); break;
                default: UNHANDLED(); break;
                }
                break;

            case 2:
                switch (operation) {
                case 1: sixel_geometry_report_current(term); break;
                case 2: sixel_geometry_reset(term); break;
                case 3: sixel_geometry_set(term, vt_param_get(term, 2, 0), vt_param_get(term, 3, 0)); break;
                case 4: sixel_geometry_report_max(term); break;
                default: UNHANDLED(); break;
                }
                break;

            default:
                UNHANDLED();
                break;
            }

            break;
        }

        case 'm': {
            int resource = vt_param_get(term, 0, 0);
            int value = -1;

            switch (resource) {
            case 0:  /* modifyKeyboard */
                value = 0;
                break;

            case 1:  /* modifyCursorKeys */
            case 2:  /* modifyFunctionKeys */
                value = 1;
                break;

            case 4:  /* modifyOtherKeys */
                value = term->modify_other_keys_2 ? 2 : 1;
                break;

            default:
                LOG_WARN("XTQMODKEYS: invalid resource '%d' in '%s'",
                         resource, csi_as_string(term, final, -1));
                break;
            }

            if (value >= 0) {
                char reply[16] = {0};
                int chars = snprintf(reply, sizeof(reply),
                                     "\033[>%d;%dm", resource, value);
                term_to_slave(term, reply, chars);
            }
            break;
        }

        case 'p': {
            /*
             * Request status of ECMA-48/"ANSI" private mode (DECRQM
             * for SM/RM modes; see private="?$" case further below for
             * DECSET/DECRST modes)
             */
            unsigned param = vt_param_get(term, 0, 0);
            unsigned status = DECRPM_NOT_RECOGNIZED;
            if (param == 4) {
                status = decrpm(term->insert_mode);
            }
            char reply[32];
            size_t n = xsnprintf(reply, sizeof(reply), "\033[%u;%u$y", param, status);
            term_to_slave(term, reply, n);
            break;
        }

        case 'u': {
            enum kitty_kbd_flags flags =
                term->grid->kitty_kbd.flags[term->grid->kitty_kbd.idx];

            char reply[8];
            int chars = snprintf(reply, sizeof(reply), "\033[?%uu", flags);
            term_to_slave(term, reply, chars);
            break;
        }

        default:
            UNHANDLED();
            break;
        }

        break; /* private[0] == '?' */
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
                size_t n = xsnprintf(reply, sizeof(reply), "\033[>1;%02u%02u%02u;0c",
                         FOOT_MAJOR, FOOT_MINOR, FOOT_PATCH);

                term_to_slave(term, reply, n);
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
                    /* Ignored, we always report modifiers */
                    if (value != 2 && value != -1) {
                        LOG_WARN(
                            "unimplemented: %s = %d",
                            resource == 1 ? "modifyCursorKeys" :
                            resource == 2 ? "modifyFunctionKeys" : "<invalid>",
                            value);
                    }
                    break;

                case 4: /* modifyOtherKeys */
                    term->modify_other_keys_2 = value == 2;
                    LOG_DBG("modifyOtherKeys=%d", value);
                    break;

                default:
                    LOG_WARN("XTMODKEYS: invalid resource '%d' in '%s'",
                             resource, csi_as_string(term, final, -1));
                    break;
                }
            }
            break; /* final == 'm' */

        case 'n': {
            int resource = vt_param_get(term, 0, 2);  /* Default is modifyFunctionKeys */
            switch (resource) {
            case 0:  /* modifyKeyboard */
            case 1:  /* modifyCursorKeys */
            case 2:  /* modifyFunctionKeys */
                break;

            case 4:  /* modifyOtherKeys */
                /* We don't support fully disabling modifyOtherKeys,
                 * but simply revert back to mode '1' */
                term->modify_other_keys_2 = false;
                LOG_DBG("modifyOtherKeys=1");
                break;
            }
            break;
        }

        case 'u': {
            int flags = vt_param_get(term, 0, 0) & KITTY_KBD_SUPPORTED;

            struct grid *grid = term->grid;
            uint8_t idx = grid->kitty_kbd.idx;

            if (idx + 1 >= ALEN(grid->kitty_kbd.flags)) {
                /* Stack full, evict oldest by wrapping around */
                idx = 0;
            } else
                idx++;

            grid->kitty_kbd.flags[idx] = flags;
            grid->kitty_kbd.idx = idx;

            LOG_DBG("kitty kbd: pushed new flags: 0x%03x", flags);
            break;
        }

        case 'q': {
            /* XTVERSION */
            if (vt_param_get(term, 0, 0) != 0) {
                UNHANDLED();
                break;
            }

            char reply[64];
            size_t n = xsnprintf(
                reply, sizeof(reply), "\033P>|foot(%u.%u.%u%s%s)\033\\",
                FOOT_MAJOR, FOOT_MINOR, FOOT_PATCH,
                FOOT_EXTRA[0] != '\0' ? "-" : "", FOOT_EXTRA);
            term_to_slave(term, reply, n);
            break;
        }

        default:
            UNHANDLED();
            break;
        }

        break; /* private[0] == '>' */
    }

    case '<': {
        switch (final) {
        case 'u': {
            int count = vt_param_get(term, 0, 1);
            LOG_DBG("kitty kbd: popping %d levels of flags", count);

            struct grid *grid = term->grid;
            uint8_t idx = grid->kitty_kbd.idx;

            for (int i = 0; i < count; i++) {
                /* Reset flags. This ensures we get flags=0 when
                 * over-popping */
                grid->kitty_kbd.flags[idx] = 0;

                if (idx == 0)
                    idx = ALEN(grid->kitty_kbd.flags) - 1;
                else
                    idx--;
            }

            grid->kitty_kbd.idx = idx;

            LOG_DBG("kitty kbd: flags after pop: 0x%03x",
                    term->grid->kitty_kbd.flags[idx]);
            break;
        }
        }
        break; /* private[0] == '<' */
    }

    case ' ': {
        switch (final) {
        case 'q': {
            int param = vt_param_get(term, 0, 0);
            switch (param) {
            case 0: /* blinking block, but we use it to reset to configured default */
                term->cursor_style = term->conf->cursor.style;
                term->cursor_blink.deccsusr = term->conf->cursor.blink.enabled;
                term_cursor_blink_update(term);
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
                term->cursor_style = CURSOR_BEAM;
                break;

            default:
                UNHANDLED();
                break;
            }

            if (param > 0 && param <= 6) {
                term->cursor_blink.deccsusr = param & 1;
                term_cursor_blink_update(term);
            }
            break;
        }

        default:
            UNHANDLED();
            break;
        }
        break; /* private[0] == ' ' */
    }

    case '!': {
        if (final == 'p') {
            term_reset(term, false);
            break;
        }

        UNHANDLED();
        break; /* private[0] == '!' */
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

        case 'u': {
            int flag_set = vt_param_get(term, 0, 0) & KITTY_KBD_SUPPORTED;
            int mode = vt_param_get(term, 1, 1);

            struct grid *grid = term->grid;
            uint8_t idx = grid->kitty_kbd.idx;

            switch (mode) {
            case 1:
                /* set bits are set, unset bits are reset */
                grid->kitty_kbd.flags[idx] = flag_set;
                break;

            case 2:
                /* set bits are set, unset bits are left unchanged */
                grid->kitty_kbd.flags[idx] |= flag_set;
                break;

            case 3:
                /* set bits are reset, unset bits are left unchanged */
                grid->kitty_kbd.flags[idx] &= ~flag_set;
                break;

            default:
                UNHANDLED();
                break;
            }

            LOG_DBG("kitty kbd: flags after update: 0x%03x",
                    grid->kitty_kbd.flags[idx]);
            break;
        }

        default:
            UNHANDLED();
            break;
        }
        break; /* private[0] == '=' */
    }

    case '$': {
        switch (final) {
        case 'r': {  /* DECCARA */
            int top, left, bottom, right;
            if (!params_to_rectangular_area(
                    term, 0, &top, &left, &bottom, &right))
            {
                break;
            }

            for (int r = top; r <= bottom; r++) {
                struct row *row = grid_row(term->grid, r);
                row->dirty = true;

                for (int c = left; c <= right; c++) {
                    struct attributes *a = &row->cells[c].attrs;
                    a->clean = 0;

                    for (size_t i = 4; i < term->vt.params.idx; i++) {
                        const int param = term->vt.params.v[i].value;

                        /* DECCARA only supports a sub-set of SGR parameters */
                        switch (param) {
                        case 0:
                            a->bold = false;
                            a->underline = false;
                            a->blink = false;
                            a->reverse = false;
                            break;

                        case 1: a->bold = true; break;
                        case 4: a->underline = true; break;
                        case 5: a->blink = true; break;
                        case 7: a->reverse = true; break;

                        case 22: a->bold = false; break;
                        case 24: a->underline = false; break;
                        case 25: a->blink = false; break;
                        case 27: a->reverse = false; break;
                        }
                    }
                }
            }
            break;
        }

        case 't': {  /* DECRARA */
            int top, left, bottom, right;
            if (!params_to_rectangular_area(
                    term, 0, &top, &left, &bottom, &right))
            {
                break;
            }

            for (int r = top; r <= bottom; r++) {
                struct row *row = grid_row(term->grid, r);
                row->dirty = true;

                for (int c = left; c <= right; c++) {
                    struct attributes *a = &row->cells[c].attrs;
                    a->clean = 0;

                    for (size_t i = 4; i < term->vt.params.idx; i++) {
                        const int param = term->vt.params.v[i].value;

                        /* DECRARA only supports a sub-set of SGR parameters */
                        switch (param) {
                        case 0:
                            a->bold = !a->bold;
                            a->underline = !a->underline;
                            a->blink = !a->blink;
                            a->reverse = !a->reverse;
                            break;

                        case 1: a->bold = !a->bold; break;
                        case 4: a->underline = !a->underline; break;
                        case 5: a->blink = !a->blink; break;
                        case 7: a->reverse = !a->reverse; break;
                        }
                    }
                }
            }
            break;
        }

        case 'v': {  /* DECCRA */
            int src_top, src_left, src_bottom, src_right;
            if (!params_to_rectangular_area(
                    term, 0, &src_top, &src_left, &src_bottom, &src_right))
            {
                break;
            }

            int src_page = vt_param_get(term, 4, 1);

            int dst_rel_top = vt_param_get(term, 5, 1) - 1;
            int dst_left = vt_param_get(term, 6, 1) - 1;
            int dst_page = vt_param_get(term, 7, 1);

            if (unlikely(src_page != 1 || dst_page != 1)) {
                /* We don’t support “pages” */
                break;
            }

            int dst_rel_bottom = dst_rel_top + (src_bottom - src_top);
            int dst_right = min(dst_left + (src_right - src_left), term->cols - 1);

            int dst_top = term_row_rel_to_abs(term, dst_rel_top);
            int dst_bottom = term_row_rel_to_abs(term, dst_rel_bottom);

            /* Target area outside the screen is clipped */
            const size_t row_count = min(src_bottom - src_top,
                                         dst_bottom - dst_top) + 1;
            const size_t cell_count = min(src_right - src_left,
                                          dst_right - dst_left) + 1;

            sixel_overwrite_by_rectangle(
                term, dst_top, dst_left, row_count, cell_count);

            /*
             * Copy source area
             *
             * Note: since source and destination may overlap, we need
             * to copy out the entire source region first, and _then_
             * write the destination. I.e. this is similar to how
             * memmove() behaves, but adapted to our row/cell
             * structure.
             */
            struct cell **copy = xmalloc(row_count * sizeof(copy[0]));
            for (int r = 0; r < row_count; r++) {
                copy[r] = xmalloc(cell_count * sizeof(copy[r][0]));

                const struct row *row = grid_row(term->grid, src_top + r);
                const struct cell *cell = &row->cells[src_left];
                memcpy(copy[r], cell, cell_count * sizeof(copy[r][0]));
            }

            /* Paste into destination area */
            for (int r = 0; r < row_count; r++) {
                struct row *row = grid_row(term->grid, dst_top + r);
                row->dirty = true;

                struct cell *cell = &row->cells[dst_left];
                memcpy(cell, copy[r], cell_count * sizeof(copy[r][0]));
                free(copy[r]);

                for (;cell < &row->cells[dst_left + cell_count]; cell++)
                    cell->attrs.clean = 0;

                if (unlikely(row->extra != NULL)) {
                    /* TODO: technically, we should copy the source URIs... */
                    grid_row_uri_range_erase(row, dst_left, dst_right);
                }
            }
            free(copy);
            break;
        }

        case 'x': {  /* DECFRA */
            const uint8_t c = vt_param_get(term, 0, 0);

            if (unlikely(!((c >= 32 && c < 126) || c >= 160)))
                break;

            int top, left, bottom, right;
            if (!params_to_rectangular_area(
                    term, 1, &top, &left, &bottom, &right))
            {
                break;
            }

            /* Erase the entire region at once (MUCH cheaper than
             * doing it row by row, or even character by
             * character). */
            sixel_overwrite_by_rectangle(
                term, top, left, bottom - top + 1, right - left + 1);

            for (int r = top; r <= bottom; r++)
                term_fill(term, r, left, c, right - left + 1, true);

            break;
        }

        case 'z': {  /* DECERA */
            int top, left, bottom, right;
            if (!params_to_rectangular_area(
                    term, 0, &top, &left, &bottom, &right))
            {
                break;
            }

            /*
             * Note: term_erase() _also_ erases sixels, but since
             * we’re forced to erase one row at a time, erasing the
             * entire sixel here is more efficient.
             */
            sixel_overwrite_by_rectangle(
                term, top, left, bottom - top + 1, right - left + 1);

            for (int r = top; r <= bottom; r++)
                term_erase(term, r, left, r, right);
            break;
        }
        }

        break; /* private[0] == ‘$’ */
    }

    case '#': {
        switch (final) {
        case 'P': { /* XTPUSHCOLORS */
            int slot = vt_param_get(term, 0, 0);

            /* Pm == 0, "push" (what xterm does is take take the
               *current* slot + 1, even if that's in the middle of the
               stack, and overwrites whatever is already in that
               slot) */
            if (slot == 0)
                slot = term->color_stack.idx + 1;

            if (term->color_stack.size < slot) {
                const size_t new_size = slot;
                term->color_stack.stack = xrealloc(
                    term->color_stack.stack,
                    new_size * sizeof(term->color_stack.stack[0]));

                /* Initialize new slots (except the selected slot,
                   which is done below) */
                xassert(new_size > 0);
                for (size_t i = term->color_stack.size; i < new_size - 1; i++) {
                    memcpy(&term->color_stack.stack[i], &term->colors,
                           sizeof(term->colors));
                }
                term->color_stack.size = new_size;
            }

            xassert(slot > 0);
            xassert(slot <= term->color_stack.size);
            term->color_stack.idx = slot;
            memcpy(&term->color_stack.stack[slot - 1], &term->colors,
                   sizeof(term->colors));
            break;
        }

        case 'Q': {  /* XTPOPCOLORS */
            int slot = vt_param_get(term, 0, 0);

            /* Pm == 0, "pop" (what xterm does is copy colors from the
              *current* slot, *and* decrease the current slot index,
              even if that's in the middle of the stack) */
            if (slot == 0)
                slot = term->color_stack.idx;

            if (slot > 0 && slot <= term->color_stack.size) {
                memcpy(&term->colors, &term->color_stack.stack[slot - 1],
                       sizeof(term->colors));
                term->color_stack.idx = slot - 1;

                /* Assume a full palette switch *will* affect almost
                   all cells. The alternative is to call
                   term_damage_color() for all 256 palette entries
                   *and* the default fg/bg (256 + 2 calls in total) */
                term_damage_view(term);
                term_damage_margins(term);
            } else if (slot == 0) {
                LOG_ERR("XTPOPCOLORS: cannot pop beyond the first element");
            } else {
                LOG_ERR(
                    "XTPOPCOLORS: invalid color slot: %d "
                    "(stack has %zu slots, current slot is %zu)",
                    vt_param_get(term, 0, 0),
                    term->color_stack.size, term->color_stack.idx);
            }
            break;
        }

        case 'R': {  /* XTREPORTCOLORS */
            char reply[64];
            size_t n = xsnprintf(reply, sizeof(reply), "\033[?%zu;%zu#Q",
                              term->color_stack.idx, term->color_stack.size);
            term_to_slave(term, reply, n);
            break;
        }
        }
        break; /* private[0] == '#' */
    }

    case 0x243f:  /* ?$ */
        switch (final) {
        case 'p': {
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
            unsigned status = decrqm(term, param);
            char reply[32];
            size_t n = xsnprintf(reply, sizeof(reply), "\033[?%u;%u$y", param, status);
            term_to_slave(term, reply, n);
            break;

        }

        default:
            UNHANDLED();
            break;
        }

        break; /* private[0] == '?' && private[1] == '$' */

    default:
        UNHANDLED();
        break;
    }
}
