#include "vt.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#define LOG_MODULE "vt"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "csi.h"
#include "dcs.h"
#include "grid.h"
#include "osc.h"
#include "util.h"
#include "xmalloc.h"

#define UNHANDLED() LOG_DBG("unhandled: %s", esc_as_string(term, final))

/* https://vt100.net/emu/dec_ansi_parser */

enum state {
    STATE_GROUND,
    STATE_ESCAPE,
    STATE_ESCAPE_INTERMEDIATE,

    STATE_CSI_ENTRY,
    STATE_CSI_PARAM,
    STATE_CSI_INTERMEDIATE,
    STATE_CSI_IGNORE,

    STATE_OSC_STRING,

    STATE_DCS_ENTRY,
    STATE_DCS_PARAM,
    STATE_DCS_INTERMEDIATE,
    STATE_DCS_IGNORE,
    STATE_DCS_PASSTHROUGH,

    STATE_SOS_PM_APC_STRING,

    STATE_UTF8_21,
    STATE_UTF8_31,
    STATE_UTF8_32,
    STATE_UTF8_41,
    STATE_UTF8_42,
    STATE_UTF8_43,
};

#if defined(_DEBUG) && defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG && 0
static const char *const state_names[] = {
    [STATE_GROUND] = "ground",

    [STATE_ESCAPE] = "escape",
    [STATE_ESCAPE_INTERMEDIATE] = "escape intermediate",

    [STATE_CSI_ENTRY] = "CSI entry",
    [STATE_CSI_PARAM] = "CSI param",
    [STATE_CSI_INTERMEDIATE] = "CSI intermediate",
    [STATE_CSI_IGNORE] = "CSI ignore",

    [STATE_OSC_STRING] = "OSC string",

    [STATE_DCS_ENTRY] = "DCS entry",
    [STATE_DCS_PARAM] = "DCS param",
    [STATE_DCS_INTERMEDIATE] = "DCS intermediate",
    [STATE_DCS_IGNORE] = "DCS ignore",
    [STATE_DCS_PASSTHROUGH] = "DCS passthrough",

    [STATE_SOS_PM_APC_STRING] = "sos/pm/apc string",

    [STATE_UTF8_21] = "UTF8 2-byte 1/2",
    [STATE_UTF8_31] = "UTF8 3-byte 1/3",
    [STATE_UTF8_32] = "UTF8 3-byte 2/3",
};
#endif

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
static const char *
esc_as_string(struct terminal *term, uint8_t final)
{
    static char msg[1024];
    int c = snprintf(msg, sizeof(msg), "\\E");

    for (size_t i = 0; i < sizeof(term->vt.private) / sizeof(term->vt.private[0]); i++) {
        if (term->vt.private[i] == 0)
            break;
        c += snprintf(&msg[c], sizeof(msg) - c, "%c", term->vt.private[i]);
    }

    assert(term->vt.params.idx == 0);

    snprintf(&msg[c], sizeof(msg) - c, "%c", final);
    return msg;

}
#endif

static void
action_ignore(struct terminal *term)
{
}

static void
action_clear(struct terminal *term)
{
    term->vt.params.idx = 0;
    term->vt.private[0] = 0;
    term->vt.private[1] = 0;
}

static void
action_execute(struct terminal *term, uint8_t c)
{
    LOG_DBG("execute: 0x%02x", c);
    switch (c) {

        /*
         * 7-bit C0 control characters
         */

    case '\0':
        break;

    case '\a':
        /* BEL - bell */
        term_bell(term);
        break;

    case '\b':
        /* backspace */
        if (term->grid->cursor.lcf)
            term->grid->cursor.lcf = false;
        else
            term_cursor_left(term, 1);
        break;

    case '\t': {
        /* HT - horizontal tab */
        int new_col = term->cols - 1;
        tll_foreach(term->tab_stops, it) {
            if (it->item > term->grid->cursor.point.col) {
                new_col = it->item;
                break;
            }
        }
        assert(new_col >= term->grid->cursor.point.col);

        /* According to the specification, HT _should_ cancel LCF. But
         * XTerm, and nearly all other emulators, don't. So we follow
         * suit */
        bool lcf = term->grid->cursor.lcf;
        term_cursor_right(term, new_col - term->grid->cursor.point.col);
        term->grid->cursor.lcf = lcf;
        break;
    }

    case '\n':
    case '\v':
    case '\f':
        /* LF - \n - line feed */
        /* VT - \v - vertical tab */
        /* FF - \f - form feed */
        term_linefeed(term);
        break;

    case '\r':
        /* CR - carriage ret */
        term_carriage_return(term);
        break;

    case '\x0e':
        /* SO - shift out */
        term->charsets.selected = 1; /* G1 */
        break;

    case '\x0f':
        /* SI - shift in */
        term->charsets.selected = 0; /* G0 */
        break;

        /*
         * 8-bit C1 control characters
         *
         * We ignore these, but keep them here for reference, along
         * with their corresponding 7-bit variants.
         *
         * As far as I can tell, XTerm also ignores these _when in
         * UTF-8 mode_. Which would be the normal mode of operation
         * these days. And since we _only_ support UTF-8...
         */

#if 0
    case '\x84':  /* IND     -> ESC D */
    case '\x85':  /* NEL     -> ESC E */
    case '\x88':  /* Tab Set -> ESC H */
    case '\x8d':  /* RI      -> ESC M */
    case '\x8e':  /* SS2     -> ESC N */
    case '\x8f':  /* SS3     -> ESC O */
    case '\x90':  /* DCS     -> ESC P */
    case '\x96':  /* SPA     -> ESC V */
    case '\x97':  /* EPA     -> ESC W */
    case '\x98':  /* SOS     -> ESC X */
    case '\x9a':  /* DECID   -> ESC Z (obsolete form of CSI c) */
    case '\x9b':  /* CSI     -> ESC [ */
    case '\x9c':  /* ST      -> ESC \ */
    case '\x9d':  /* OSC     -> ESC ] */
    case '\x9e':  /* PM      -> ESC ^ */
    case '\x9f':  /* APC     -> ESC _ */
        break;
#endif

    default:
        break;
    }
}

static void
action_print(struct terminal *term, uint8_t c)
{
    /* 0x60 - 0x7e */
    static const wchar_t vt100_0[] = {
        L'◆', L'▒', L'␉', L'␌', L'␍', L'␊', L'°', L'±', /* ` - g */
        L'␤', L'␋', L'┘', L'┐', L'┌', L'└', L'┼', L'⎺', /* h - o */
        L'⎻', L'─', L'⎼', L'⎽', L'├', L'┤', L'┴', L'┬', /* p - w */
        L'│', L'≤', L'≥', L'π', L'≠', L'£', L'·',       /* x - ~ */
    };

    assert(wcwidth(c) == 1);

    if (unlikely(term->charsets.set[term->charsets.selected] == CHARSET_GRAPHIC) &&
        c >= 0x60 && c <= 0x7e)
    {
        term_print(term, vt100_0[c - 0x60], 1);
    } else {
        term_print(term, c, 1);
    }
}

static void
action_param(struct terminal *term, uint8_t c)
{
    if (term->vt.params.idx == 0) {
        struct vt_param *param = &term->vt.params.v[0];
        param->value = 0;
        param->sub.idx = 0;
        term->vt.params.idx = 1;
    }

    assert(term->vt.params.idx > 0);

    const size_t max_params
        = sizeof(term->vt.params.v) / sizeof(term->vt.params.v[0]);
    const size_t max_sub_params
        = sizeof(term->vt.params.v[0].sub.value) / sizeof(term->vt.params.v[0].sub.value[0]);

    /* New parameter */
    if (c == ';') {
        if (unlikely(term->vt.params.idx >= max_params))
            goto excess_params;

        struct vt_param *param = &term->vt.params.v[term->vt.params.idx++];
        param->value = 0;
        param->sub.idx = 0;
    }

    /* New sub-parameter */
    else if (c == ':') {
        if (unlikely(term->vt.params.idx - 1 >= max_params))
            goto excess_params;

        struct vt_param *param = &term->vt.params.v[term->vt.params.idx - 1];
        if (unlikely(param->sub.idx >= max_sub_params))
            goto excess_sub_params;

        param->sub.value[param->sub.idx++] = 0;
    }

    /* New digit for current parameter/sub-parameter */
    else {
        if (unlikely(term->vt.params.idx - 1 >= max_params))
            goto excess_params;

        struct vt_param *param = &term->vt.params.v[term->vt.params.idx - 1];
        unsigned *value;

        if (param->sub.idx > 0) {
            if (unlikely(param->sub.idx - 1 >= max_sub_params))
                goto excess_sub_params;
            value = &param->sub.value[param->sub.idx - 1];
        } else
            value = &param->value;

        *value *= 10;
        *value += c - '0';
    }

#if defined(_DEBUG)
    /* The rest of the code assumes 'idx' *never* points outside the array */
    assert(term->vt.params.idx <= max_params);
    for (size_t i = 0; i < term->vt.params.idx; i++)
        assert(term->vt.params.v[i].sub.idx <= max_sub_params);
#endif

    return;

excess_params:
    {
        static bool have_warned = false;
        if (!have_warned) {
            have_warned = true;
            LOG_WARN(
                "unsupported: escape with more than %zu parameters "
                "(will not warn again)",
                sizeof(term->vt.params.v) / sizeof(term->vt.params.v[0]));
        }
    }
    return;

excess_sub_params:
    {
        static bool have_warned = false;
        if (!have_warned) {
            have_warned = true;
            LOG_WARN(
                "unsupported: escape with more than %zu sub-parameters "
                "(will not warn again)",
                sizeof(term->vt.params.v[0].sub.value) / sizeof(term->vt.params.v[0].sub.value[0]));
        }
    }
    return;
}

static void
action_collect(struct terminal *term, uint8_t c)
{
    LOG_DBG("collect: %c", c);
    if (term->vt.private[0] == 0)
        term->vt.private[0] = c;
    else if (term->vt.private[1] == 0)
        term->vt.private[1] = c;
    else
        LOG_WARN("only two private/intermediate characters supported");
}

static void
action_esc_dispatch(struct terminal *term, uint8_t final)
{
    LOG_DBG("ESC: %s", esc_as_string(term, final));

    switch (term->vt.private[0]) {
    case 0:
        switch (final) {
        case '7':
            term->grid->saved_cursor = term->grid->cursor;
            term->vt.saved_attrs = term->vt.attrs;
            term->saved_charsets = term->charsets;
            break;

        case '8':
            term_restore_cursor(term, &term->grid->saved_cursor);
            term->vt.attrs = term->vt.saved_attrs;
            term->charsets = term->saved_charsets;
            break;

        case 'c':
            term_reset(term, true);
            break;

        case 'D':
            term_linefeed(term);
            break;

        case 'E':
            term_carriage_return(term);
            term_linefeed(term);
            break;

        case 'H':
            tll_foreach(term->tab_stops, it) {
                if (it->item >= term->grid->cursor.point.col) {
                    tll_insert_before(term->tab_stops, it, term->grid->cursor.point.col);
                    break;
                }
            }

            tll_push_back(term->tab_stops, term->grid->cursor.point.col);
            break;

        case 'M':
            term_reverse_index(term);
            break;

        case 'N':
            /* SS2 - Single Shift 2 */
            term->charsets.selected = 2; /* G2 */
            break;

        case 'O':
            /* SS3 - Single Shift 3 */
            term->charsets.selected = 3; /* G3 */
            break;

        case '\\':
            /* ST - String Terminator */
            break;

        case '=':
            term->keypad_keys_mode = KEYPAD_APPLICATION;
            break;

        case '>':
            term->keypad_keys_mode = KEYPAD_NUMERICAL;
            break;

        default:
            UNHANDLED();
            break;
        }
        break;  /* private[0] == 0 */

    case '(':
    case ')':
    case '*':
    case '+':
        switch (final) {
        case '0': {
            char priv = term->vt.private[0];
            ssize_t idx = priv ==
                '(' ? 0 :
                ')' ? 1 :
                '*' ? 2 :
                '+' ? 3 : -1;
            assert(idx != -1);
            term->charsets.set[idx] = CHARSET_GRAPHIC;
            break;
        }

        case 'B': {
            char priv = term->vt.private[0];
            ssize_t idx = priv ==
                '(' ? 0 :
                ')' ? 1 :
                '*' ? 2 :
                '+' ? 3 : -1;
            assert(idx != -1);
            term->charsets.set[idx] = CHARSET_ASCII;

            break;
        }
        }
        break;

    case '#':
        switch (final) {
        case '8':
            for (int r = 0; r < term->rows; r++) {
                struct row *row = grid_row(term->grid, r);
                for (int c = 0; c < term->cols; c++) {
                    row->cells[c].wc = L'E';
                    row->cells[c].attrs.clean = 0;
                }
                row->dirty = true;
            }
            break;
        }
        break;  /* private[0] == '#' */

    }
}

static void
action_csi_dispatch(struct terminal *term, uint8_t c)
{
    csi_dispatch(term, c);
}

static void
action_osc_start(struct terminal *term, uint8_t c)
{
    term->vt.osc.idx = 0;
}

static void
action_osc_end(struct terminal *term, uint8_t c)
{
    if (!osc_ensure_size(term, term->vt.osc.idx + 1))
        return;
    term->vt.osc.data[term->vt.osc.idx] = '\0';
    osc_dispatch(term);
}

static void
action_osc_put(struct terminal *term, uint8_t c)
{
    if (!osc_ensure_size(term, term->vt.osc.idx + 1))
        return;
    term->vt.osc.data[term->vt.osc.idx++] = c;
}

static void
action_hook(struct terminal *term, uint8_t c)
{
    dcs_hook(term, c);
}

static void
action_unhook(struct terminal *term, uint8_t c)
{
    dcs_unhook(term);
}

static void
action_put(struct terminal *term, uint8_t c)
{
    dcs_put(term, c);
}

static void
action_utf8_print(struct terminal *term, wchar_t wc)
{
    int width = wcwidth(wc);

    /*
     * Is this is combining character? The basic assumption is that if
     * wcwdith() returns 0, then it *is* a combining character.
     *
     * We hen optimize this by ignoring all characters before 0x0300,
     * since there aren't any zero-width characters there. This means
     * all "normal" western characters will quickly be categorized as
     * *not* being combining characters.
     *
     * TODO: xterm does more or less the same, but also filters a
     * small subset of BIDI control characters. Should we too? I think
     * what we have here is good enough - a control character
     * shouldn't have a glyph associated with it, so rendering
     * shouldn't be affected.
     *
     * TODO: handle line-wrap when locating the base character.
     */
    if (width == 0 && wc >= 0x0300 && term->grid->cursor.point.col > 0) {
        const struct row *row = term->grid->cur_row;

        int base_col = term->grid->cursor.point.col;
        if (!term->grid->cursor.lcf)
            base_col--;

        while (row->cells[base_col].wc == CELL_MULT_COL_SPACER && base_col > 0)
            base_col--;

        assert(base_col >= 0 && base_col < term->cols);
        wchar_t base = row->cells[base_col].wc;

        const struct composed *composed =
            (base >= CELL_COMB_CHARS_LO &&
             base < (CELL_COMB_CHARS_LO + term->composed_count))
            ? &term->composed[base - CELL_COMB_CHARS_LO]
            : NULL;

        if (composed != NULL)
            base = composed->base;

        int base_width = wcwidth(base);

        if (base != 0 && base_width > 0) {

            /*
             * If this is the *first* combining characger, see if
             * there's a pre-composed character of this combo, with
             * the same column width as the base character.
             *
             * If there is, replace the base character with the
             * pre-composed character, as that is likely to produce a
             * better looking result.
             */
            term->grid->cursor.point.col = base_col;
            term->grid->cursor.lcf = false;

            if (composed == NULL) {
                bool base_from_primary;
                bool comb_from_primary;
                bool pre_from_primary;

                wchar_t precomposed = fcft_precompose(
                    term->fonts[0], base, wc, &base_from_primary,
                    &comb_from_primary, &pre_from_primary);

                int precomposed_width = wcwidth(precomposed);

                /*
                 * Only use the pre-composed character if:
                 *
                 *  1. we *have* a pre-composed character
                 *  2. the width matches the base characters width
                 *  3. it's in the primary font, OR one of the base or
                 *     combining characters are *not* from the primary
                 *     font
                 */

                if (precomposed != (wchar_t)-1 &&
                    precomposed_width == base_width &&
                    (pre_from_primary ||
                     !base_from_primary ||
                     !comb_from_primary))
                {
                    term_print(term, precomposed, precomposed_width);
                    return;
                }
            }

            size_t wanted_count = composed != NULL ? composed->count + 1 : 1;
            if (wanted_count > ALEN(composed->combining)) {
                assert(composed != NULL);

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
                LOG_WARN("combining character overflow:");
                LOG_WARN("  base: 0x%04x", composed->base);
                for (size_t i = 0; i < composed->count; i++)
                    LOG_WARN("    cc: 0x%04x", composed->combining[i]);
                LOG_ERR("   new: 0x%04x", wc);
#endif
                /* This are going to break anyway... */
                wanted_count--;
            }

            assert(wanted_count <= ALEN(composed->combining));

            /* Look for existing combining chain */
            for (size_t i = 0; i < term->composed_count; i++) {
                const struct composed *cc = &term->composed[i];
                if (cc->base != base)
                    continue;

                if (cc->count != wanted_count)
                    continue;

                if (cc->combining[wanted_count - 1] != wc)
                    continue;

                term_print(term, CELL_COMB_CHARS_LO + i, base_width);
                return;
            }

            /* Allocate new chain */

            struct composed new_cc;
            new_cc.base = base;
            new_cc.count = wanted_count;
            for (size_t i = 0; i < wanted_count - 1; i++)
                new_cc.combining[i] = composed->combining[i];
            new_cc.combining[wanted_count - 1] = wc;

            if (term->composed_count < CELL_COMB_CHARS_HI) {
                term->composed_count++;
                term->composed = xrealloc(term->composed, term->composed_count * sizeof(term->composed[0]));
                term->composed[term->composed_count - 1] = new_cc;

                term_print(term, CELL_COMB_CHARS_LO + term->composed_count - 1, base_width);
                return;
            } else {
                /* We reached our maximum number of allowed composed
                 * character chains. Fall through here and print the
                 * current zero-width character to the current cell */
                LOG_WARN("maximum number of composed characters reached");
            }
        }
    }

    if (width > 0)
        term_print(term, wc, width);
}

static void
action_utf8_21(struct terminal *term, uint8_t c)
{
    // wc = ((utf8[0] & 0x1f) << 6) | (utf8[1] & 0x3f)
    term->vt.utf8 = (c & 0x1f) << 6;
}

static void
action_utf8_22(struct terminal *term, uint8_t c)
{
    // wc = ((utf8[0] & 0x1f) << 6) | (utf8[1] & 0x3f)
    term->vt.utf8 |= c & 0x3f;
    action_utf8_print(term, term->vt.utf8);
}

static void
action_utf8_31(struct terminal *term, uint8_t c)
{
    // wc = ((utf8[0] & 0xf) << 12) | ((utf8[1] & 0x3f) << 6) | (utf8[2] & 0x3f)
    term->vt.utf8 = (c & 0x0f) << 12;
}

static void
action_utf8_32(struct terminal *term, uint8_t c)
{
    // wc = ((utf8[0] & 0xf) << 12) | ((utf8[1] & 0x3f) << 6) | (utf8[2] & 0x3f)
    term->vt.utf8 |= (c & 0x3f) << 6;
}

static void
action_utf8_33(struct terminal *term, uint8_t c)
{
    // wc = ((utf8[0] & 0xf) << 12) | ((utf8[1] & 0x3f) << 6) | (utf8[2] & 0x3f)
    term->vt.utf8 |= c & 0x3f;
    action_utf8_print(term, term->vt.utf8);
}

static void
action_utf8_41(struct terminal *term, uint8_t c)
{
    // wc = ((utf8[0] & 7) << 18) | ((utf8[1] & 0x3f) << 12) | ((utf8[2] & 0x3f) << 6) | (utf8[3] & 0x3f);
    term->vt.utf8 = (c & 0x07) << 18;
}

static void
action_utf8_42(struct terminal *term, uint8_t c)
{
    // wc = ((utf8[0] & 7) << 18) | ((utf8[1] & 0x3f) << 12) | ((utf8[2] & 0x3f) << 6) | (utf8[3] & 0x3f);
    term->vt.utf8 |= (c & 0x3f) << 12;
}

static void
action_utf8_43(struct terminal *term, uint8_t c)
{
    // wc = ((utf8[0] & 7) << 18) | ((utf8[1] & 0x3f) << 12) | ((utf8[2] & 0x3f) << 6) | (utf8[3] & 0x3f);
    term->vt.utf8 |= (c & 0x3f) << 6;
}

static void
action_utf8_44(struct terminal *term, uint8_t c)
{
    // wc = ((utf8[0] & 7) << 18) | ((utf8[1] & 0x3f) << 12) | ((utf8[2] & 0x3f) << 6) | (utf8[3] & 0x3f);
    term->vt.utf8 |= c & 0x3f;
    action_utf8_print(term, term->vt.utf8);
}

#if defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wpedantic"
#endif

static enum state
state_ground_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                  action_execute(term, data);                                       return STATE_GROUND;

    /* modified from 0x20..0x7f to 0x20..0x7e, since 0x7f is DEL, which is a zero-width character */
    case 0x20 ... 0x7e:                                  action_print(term, data);                                         return STATE_GROUND;

    case 0xc2 ... 0xdf:                                  action_utf8_21(term, data);                                       return STATE_UTF8_21;
    case 0xe0 ... 0xef:                                  action_utf8_31(term, data);                                       return STATE_UTF8_31;
    case 0xf0 ... 0xf4:                                  action_utf8_41(term, data);                                       return STATE_UTF8_41;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_GROUND;
    }
}

static enum state
state_escape_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                  action_execute(term, data);                                       return STATE_ESCAPE;

    case 0x20 ... 0x2f:                                  action_collect(term, data);                                       return STATE_ESCAPE_INTERMEDIATE;
    case 0x30 ... 0x4f:                                  action_esc_dispatch(term, data);                                  return STATE_GROUND;
    case 0x50:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x51 ... 0x57:                                  action_esc_dispatch(term, data);                                  return STATE_GROUND;
    case 0x58:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x59:                                           action_esc_dispatch(term, data);                                  return STATE_GROUND;
    case 0x5a:                                           action_esc_dispatch(term, data);                                  return STATE_GROUND;
    case 0x5b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x5c:                                           action_esc_dispatch(term, data);                                  return STATE_GROUND;
    case 0x5d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x5e ... 0x5f:                                                                                                    return STATE_SOS_PM_APC_STRING;
    case 0x60 ... 0x7e:                                  action_esc_dispatch(term, data);                                  return STATE_GROUND;
    case 0x7f:                                           action_ignore(term);                                              return STATE_ESCAPE;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_ESCAPE;
    }
}

static enum state
state_escape_intermediate_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                  action_execute(term, data);                                       return STATE_ESCAPE_INTERMEDIATE;

    case 0x20 ... 0x2f:                                  action_collect(term, data);                                       return STATE_ESCAPE_INTERMEDIATE;
    case 0x30 ... 0x7e:                                  action_esc_dispatch(term, data);                                  return STATE_GROUND;
    case 0x7f:                                           action_ignore(term);                                              return STATE_ESCAPE_INTERMEDIATE;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_ESCAPE_INTERMEDIATE;
    }
}

static enum state
state_csi_entry_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                  action_execute(term, data);                                       return STATE_CSI_ENTRY;

    case 0x20 ... 0x2f:                                  action_collect(term, data);                                       return STATE_CSI_INTERMEDIATE;
    case 0x30 ... 0x39:                                  action_param(term, data);                                         return STATE_CSI_PARAM;
    case 0x3a ... 0x3b:                                  action_param(term, data);                                         return STATE_CSI_PARAM;
    case 0x3c ... 0x3f:                                  action_collect(term, data);                                       return STATE_CSI_PARAM;
    case 0x40 ... 0x7e:                                  action_csi_dispatch(term, data);                                  return STATE_GROUND;
    case 0x7f:                                           action_ignore(term);                                              return STATE_CSI_ENTRY;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_CSI_ENTRY;
    }
}

static enum state
state_csi_param_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                  action_execute(term, data);                                       return STATE_CSI_PARAM;

    case 0x20 ... 0x2f:                                  action_collect(term, data);                                       return STATE_CSI_INTERMEDIATE;

    case 0x30 ... 0x39:
    case 0x3a ... 0x3b:                                  action_param(term, data);                                         return STATE_CSI_PARAM;

    case 0x3c ... 0x3f:                                                                                                    return STATE_CSI_IGNORE;
    case 0x40 ... 0x7e:                                  action_csi_dispatch(term, data);                                  return STATE_GROUND;
    case 0x7f:                                           action_ignore(term);                                              return STATE_CSI_PARAM;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_CSI_PARAM;
    }
}

static enum state
state_csi_intermediate_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                  action_execute(term, data);                                       return STATE_CSI_INTERMEDIATE;

    case 0x20 ... 0x2f:                                  action_collect(term, data);                                       return STATE_CSI_INTERMEDIATE;
    case 0x30 ... 0x3f:                                                                                                    return STATE_CSI_IGNORE;
    case 0x40 ... 0x7e:                                  action_csi_dispatch(term, data);                                  return STATE_GROUND;
    case 0x7f:                                           action_ignore(term);                                              return STATE_CSI_INTERMEDIATE;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_CSI_INTERMEDIATE;
    }
}

static enum state
state_csi_ignore_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                  action_execute(term, data);                                       return STATE_CSI_IGNORE;

    case 0x20 ... 0x3f:                                  action_ignore(term);                                              return STATE_CSI_IGNORE;
    case 0x40 ... 0x7e:                                                                                                    return STATE_GROUND;
    case 0x7f:                                           action_ignore(term);                                              return STATE_CSI_IGNORE;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_CSI_IGNORE;
    }
}

static enum state
state_osc_string_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */

    /* Note: original was 20-7f, but I changed to 20-ff to include utf-8. Don't forget to add EXECUTE to 8-bit C1 if we implement that. */
    default:                                             action_osc_put(term, data);                                       return STATE_OSC_STRING;

    case 0x07:          action_osc_end(term, data);                                                                        return STATE_GROUND;

    case 0x00 ... 0x06:
    case 0x08 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                  action_ignore(term);                                              return STATE_OSC_STRING;


    case 0x18:
    case 0x1a:          action_osc_end(term, data);      action_execute(term, data);                                       return STATE_GROUND;

    case 0x1b:          action_osc_end(term, data);      action_clear(term);                                               return STATE_ESCAPE;
    }
}

static enum state
state_dcs_entry_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                  action_ignore(term);                                              return STATE_DCS_ENTRY;

    case 0x20 ... 0x2f:                                  action_collect(term, data);                                       return STATE_DCS_INTERMEDIATE;
    case 0x30 ... 0x39:                                  action_param(term, data);                                         return STATE_DCS_PARAM;
    case 0x3a:                                                                                                             return STATE_DCS_IGNORE;
    case 0x3b:                                           action_param(term, data);                                         return STATE_DCS_PARAM;
    case 0x3c ... 0x3f:                                  action_collect(term, data);                                       return STATE_DCS_PARAM;
    case 0x40 ... 0x7e:                                                                   action_hook(term, data);         return STATE_DCS_PASSTHROUGH;
    case 0x7f:                                           action_ignore(term);                                              return STATE_DCS_ENTRY;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_DCS_ENTRY;
    }
}

static enum state
state_dcs_param_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                  action_ignore(term);                                              return STATE_DCS_PARAM;

    case 0x20 ... 0x2f:                                  action_collect(term, data);                                       return STATE_DCS_INTERMEDIATE;
    case 0x30 ... 0x39:                                  action_param(term, data);                                         return STATE_DCS_PARAM;
    case 0x3a:                                                                                                             return STATE_DCS_IGNORE;
    case 0x3b:                                           action_param(term, data);                                         return STATE_DCS_PARAM;
    case 0x3c ... 0x3f:                                                                                                    return STATE_DCS_IGNORE;
    case 0x40 ... 0x7e:                                                                   action_hook(term, data);         return STATE_DCS_PASSTHROUGH;
    case 0x7f:                                           action_ignore(term);                                              return STATE_DCS_PARAM;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_DCS_PARAM;
    }
}

static enum state
state_dcs_intermediate_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                  action_ignore(term);                                              return STATE_DCS_INTERMEDIATE;

    case 0x20 ... 0x2f:                                  action_collect(term, data);                                       return STATE_DCS_INTERMEDIATE;
    case 0x30 ... 0x3f:                                                                                                    return STATE_DCS_IGNORE;
    case 0x40 ... 0x7e:                                                                   action_hook(term, data);         return STATE_DCS_PASSTHROUGH;
    case 0x7f:                                           action_ignore(term);                                              return STATE_DCS_INTERMEDIATE;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_DCS_INTERMEDIATE;
    }
}

static enum state
state_dcs_ignore_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:
    case 0x20 ... 0x7f:                                  action_ignore(term);                                              return STATE_DCS_IGNORE;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_DCS_IGNORE;
    }
}

static enum state
state_dcs_passthrough_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x7e:                                  action_put(term, data);                                           return STATE_DCS_PASSTHROUGH;

    case 0x7f:                                           action_ignore(term);                                              return STATE_DCS_PASSTHROUGH;

    /* Anywhere */
    case 0x18:          action_unhook(term, data);       action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:          action_unhook(term, data);       action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:          action_unhook(term, data);                                        action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f: action_unhook(term, data);       action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:          action_unhook(term, data);                                        action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97: action_unhook(term, data);       action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:          action_unhook(term, data);                                                                         return STATE_SOS_PM_APC_STRING;
    case 0x99:          action_unhook(term, data);       action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:          action_unhook(term, data);       action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:          action_unhook(term, data);                                        action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:          action_unhook(term, data);                                                                         return STATE_GROUND;
    case 0x9d:          action_unhook(term, data);                                        action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f: action_unhook(term, data);                                                                         return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_DCS_PASSTHROUGH;
    }
}

static enum state
state_sos_pm_apc_string_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x7f:                                  action_ignore(term);                                              return STATE_SOS_PM_APC_STRING;

    /* Anywhere */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x90:                                                                            action_clear(term);              return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                  action_execute(term, data);                                       return STATE_GROUND;
    case 0x98:                                                                                                             return STATE_SOS_PM_APC_STRING;
    case 0x99:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x9b:                                                                            action_clear(term);              return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                             return STATE_GROUND;
    case 0x9d:                                                                            action_osc_start(term, data);    return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                    return STATE_SOS_PM_APC_STRING;

    default:                                                                                                               return STATE_SOS_PM_APC_STRING;
    }
}

static enum state
state_utf8_21_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_22(term, data);                                       return STATE_GROUND;
    default:                                             action_utf8_print(term, 0);                                       return STATE_GROUND;
    }
}

static enum state
state_utf8_31_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_32(term, data);                                       return STATE_UTF8_32;
    default:                                             action_utf8_print(term, 0);                                       return STATE_GROUND;
    }
}

static enum state
state_utf8_32_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_33(term, data);                                       return STATE_GROUND;
    default:                                             action_utf8_print(term, 0);                                       return STATE_GROUND;
    }
}

static enum state
state_utf8_41_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_42(term, data);                                       return STATE_UTF8_42;
    default:                                             action_utf8_print(term, 0);                                       return STATE_GROUND;
    }
}

static enum state
state_utf8_42_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_43(term, data);                                       return STATE_UTF8_43;
    default:                                             action_utf8_print(term, 0);                                       return STATE_GROUND;
    }
}

static enum state
state_utf8_43_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_44(term, data);                                       return STATE_GROUND;
    default:                                             action_utf8_print(term, 0);                                       return STATE_GROUND;
    }
}

#if defined(__GNUC__)
 #pragma GCC diagnostic pop
#endif

void
vt_from_slave(struct terminal *term, const uint8_t *data, size_t len)
{
    enum state current_state = term->vt.state;

    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++, p++) {
        switch (current_state) {
        case STATE_GROUND:              current_state = state_ground_switch(term, *p); break;
        case STATE_ESCAPE:              current_state = state_escape_switch(term, *p); break;
        case STATE_ESCAPE_INTERMEDIATE: current_state = state_escape_intermediate_switch(term, *p); break;
        case STATE_CSI_ENTRY:           current_state = state_csi_entry_switch(term, *p); break;
        case STATE_CSI_PARAM:           current_state = state_csi_param_switch(term, *p); break;
        case STATE_CSI_INTERMEDIATE:    current_state = state_csi_intermediate_switch(term, *p); break;
        case STATE_CSI_IGNORE:          current_state = state_csi_ignore_switch(term, *p); break;
        case STATE_OSC_STRING:          current_state = state_osc_string_switch(term, *p); break;
        case STATE_DCS_ENTRY:           current_state = state_dcs_entry_switch(term, *p); break;
        case STATE_DCS_PARAM:           current_state = state_dcs_param_switch(term, *p); break;
        case STATE_DCS_INTERMEDIATE:    current_state = state_dcs_intermediate_switch(term, *p); break;
        case STATE_DCS_IGNORE:          current_state = state_dcs_ignore_switch(term, *p); break;
        case STATE_DCS_PASSTHROUGH:     current_state = state_dcs_passthrough_switch(term, *p); break;
        case STATE_SOS_PM_APC_STRING:   current_state = state_sos_pm_apc_string_switch(term, *p); break;

        case STATE_UTF8_21:             current_state = state_utf8_21_switch(term, *p); break;
        case STATE_UTF8_31:             current_state = state_utf8_31_switch(term, *p); break;
        case STATE_UTF8_32:             current_state = state_utf8_32_switch(term, *p); break;
        case STATE_UTF8_41:             current_state = state_utf8_41_switch(term, *p); break;
        case STATE_UTF8_42:             current_state = state_utf8_42_switch(term, *p); break;
        case STATE_UTF8_43:             current_state = state_utf8_43_switch(term, *p); break;
        }
    }


    term->vt.state = current_state;
}
