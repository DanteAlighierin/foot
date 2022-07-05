#include "vt.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(FOOT_GRAPHEME_CLUSTERING)
 #include <utf8proc.h>
#endif

#define LOG_MODULE "vt"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "config.h"
#include "csi.h"
#include "dcs.h"
#include "debug.h"
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

    for (size_t i = 0; i < sizeof(term->vt.private); i++) {
        char value = (term->vt.private >> (i * 8)) & 0xff;
        if (value == 0)
            break;
        c += snprintf(&msg[c], sizeof(msg) - c, "%c", value);
    }

    xassert(term->vt.params.idx == 0);

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
    term->vt.private = 0;
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
#if 0
        /*
         * This is the “correct” BS behavior. However, it doesn’t play
         * nicely with bw/auto_left_margin, hence the alternative
         * implementation below.
         *
         * Note that it breaks vttest “1. Test of cursor movements ->
         * Test of autowrap”
         */
        term_cursor_left(term, 1);
#else
        if (term->grid->cursor.lcf)
            term->grid->cursor.lcf = false;
        else {
            /* Reverse wrap */
            if (unlikely(term->grid->cursor.point.col == 0) &&
                likely(term->reverse_wrap && term->auto_margin))
            {
                if (term->grid->cursor.point.row <= term->scroll_region.start) {
                    /* Don’t wrap past, or inside, the scrolling region(?) */
                } else
                    term_cursor_to(
                        term,
                        term->grid->cursor.point.row - 1,
                        term->cols - 1);
            } else
                term_cursor_left(term, 1);
        }
#endif
        break;

    case '\t': {
        /* HT - horizontal tab */
        int start_col = term->grid->cursor.point.col;
        int new_col = term->cols - 1;

        tll_foreach(term->tab_stops, it) {
            if (it->item > start_col) {
                new_col = it->item;
                break;
            }
        }
        xassert(new_col >= start_col);
        xassert(new_col < term->cols);

        struct row *row = term->grid->cur_row;

        bool emit_tab_char = (row->cells[start_col].wc == 0 ||
                              row->cells[start_col].wc == U' ');

        /* Check if all cells from here until the next tab stop are empty */
        for (const struct cell *cell = &row->cells[start_col + 1];
             cell < &row->cells[new_col];
             cell++)
        {
            if (!(cell->wc == 0 || cell->wc == U' ')) {
                emit_tab_char = false;
                break;
            }
        }

        /*
         * Emit a tab in current cell, and write spaces to the
         * subsequent cells, all the way until the next tab stop.
         */
        if (emit_tab_char) {
            row->dirty = true;

            row->cells[start_col].wc = U'\t';
            row->cells[start_col].attrs.clean = 0;

            for (struct cell *cell = &row->cells[start_col + 1];
                 cell < &row->cells[new_col];
                 cell++)
            {
                cell->wc = U' ';
                cell->attrs.clean = 0;
            }
        }

        /* According to the specification, HT _should_ cancel LCF. But
         * XTerm, and nearly all other emulators, don't. So we follow
         * suit */
        bool lcf = term->grid->cursor.lcf;
        term_cursor_right(term, new_col - start_col);
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
        term->charsets.selected = G1;
        term_update_ascii_printer(term);
        break;

    case '\x0f':
        /* SI - shift in */
        term->charsets.selected = G0;
        term_update_ascii_printer(term);
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
    term_reset_grapheme_state(term);
    term->ascii_printer(term, c);
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

    xassert(term->vt.params.idx > 0);

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
    xassert(term->vt.params.idx <= max_params);
    for (size_t i = 0; i < term->vt.params.idx; i++)
        xassert(term->vt.params.v[i].sub.idx <= max_sub_params);
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

    /*
     * Having more than one private is *very* rare. Foot only supports
     * a *single* escape with two privates, and none with three or
     * more.
     *
     * As such, we optimize *reading* the private(s), and *resetting*
     * them (in action_clear()). Writing is ok if it’s a bit slow.
     */

    if ((term->vt.private & 0xff) == 0)
        term->vt.private = c;
    else if (((term->vt.private >> 8) & 0xff) == 0)
        term->vt.private |= c << 8;
    else if (((term->vt.private >> 16) & 0xff) == 0)
        term->vt.private |= c << 16;
    else if (((term->vt.private >> 24) & 0xff) == 0)
        term->vt.private |= c << 24;
    else
        LOG_WARN("only four private/intermediate characters supported");
}

UNITTEST
{
    struct terminal term = {.vt = {.private = 0}};
    uint32_t expected = ' ';
    action_collect(&term, ' ');
    xassert(term.vt.private == expected);

    expected |= '/' << 8;
    action_collect(&term, '/');
    xassert(term.vt.private == expected);

    expected |= '<' << 16;
    action_collect(&term, '<');
    xassert(term.vt.private == expected);

    expected |= '?' << 24;
    action_collect(&term, '?');
    xassert(term.vt.private == expected);

    action_collect(&term, '?');
    xassert(term.vt.private == expected);
}

static void
action_esc_dispatch(struct terminal *term, uint8_t final)
{
    LOG_DBG("ESC: %s", esc_as_string(term, final));

    switch (term->vt.private) {
    case 0:
        switch (final) {
        case '7':
            term_save_cursor(term);
            break;

        case '8':
            term_restore_cursor(term, &term->grid->saved_cursor);
            break;

        case 'c':
            term_reset(term, true);
            break;

        case 'n':
            /* LS2 - Locking Shift 2 */
            term->charsets.selected = G2;
            term_update_ascii_printer(term);
            break;

        case 'o':
            /* LS3 - Locking Shift 3 */
            term->charsets.selected = G3;
            term_update_ascii_printer(term);
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
            term_single_shift(term, G2);
            break;

        case 'O':
            /* SS3 - Single Shift 3 */
            term_single_shift(term, G3);
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

    // Designate character set
    case '(': // G0
    case ')': // G1
    case '*': // G2
    case '+': // G3
        switch (final) {
        case '0': {
            size_t idx = term->vt.private - '(';
            xassert(idx <= G3);
            term->charsets.set[idx] = CHARSET_GRAPHIC;
            term_update_ascii_printer(term);
            break;
        }

        case 'B': {
            size_t idx = term->vt.private - '(';
            xassert(idx <= G3);
            term->charsets.set[idx] = CHARSET_ASCII;
            term_update_ascii_printer(term);
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
                    row->cells[c].wc = U'E';
                    row->cells[c].attrs = (struct attributes){0};
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
    struct vt *vt = &term->vt;

    if (!osc_ensure_size(term, vt->osc.idx + 1))
        return;

    vt->osc.data[vt->osc.idx] = '\0';
    vt->osc.bel = c == '\a';
    osc_dispatch(term);

    if (unlikely(vt->osc.idx >= 4096)) {
        free(vt->osc.data);
        vt->osc.data = NULL;
        vt->osc.size = 0;
    }
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

static inline uint32_t
chain_key(uint32_t old_key, uint32_t new_wc)
{
    unsigned bits = 32 - __builtin_clz(CELL_COMB_CHARS_HI - CELL_COMB_CHARS_LO);

    /* Rotate old key 8 bits */
    uint32_t new_key = (old_key << 8) | (old_key >> (bits - 8));

    /* xor with new char */
    new_key ^= new_wc;

    /* Multiply with magic hash constant */
    new_key *= 2654435761;

    /* And mask, to ensure the new value is within range */
    new_key &= CELL_COMB_CHARS_HI - CELL_COMB_CHARS_LO;

    return new_key;
}

static void
action_utf8_print(struct terminal *term, char32_t wc)
{
    int width = c32width(wc);
    const bool grapheme_clustering = term->conf->tweak.grapheme_shaping;

#if !defined(FOOT_GRAPHEME_CLUSTERING)
    xassert(!grapheme_clustering);
#endif

    if (term->grid->cursor.point.col > 0 &&
        (grapheme_clustering ||
         (!grapheme_clustering && width == 0 && wc >= 0x300)))
    {
        int col = term->grid->cursor.point.col;
        if (!term->grid->cursor.lcf)
            col--;

        /* Skip past spacers */
        struct row *row = term->grid->cur_row;
        while (row->cells[col].wc >= CELL_SPACER && col > 0)
            col--;

        xassert(col >= 0 && col < term->cols);
        char32_t base = row->cells[col].wc;
        char32_t UNUSED last = base;

        /* Is base cell already a cluster? */
        const struct composed *composed =
            (base >= CELL_COMB_CHARS_LO && base <= CELL_COMB_CHARS_HI)
            ? composed_lookup(term->composed, base - CELL_COMB_CHARS_LO)
            : NULL;

        uint32_t key;

        if (composed != NULL) {
            base = composed->chars[0];
            last = composed->chars[composed->count - 1];
            key = chain_key(composed->key, wc);
        } else
            key = chain_key(base, wc);

#if defined(FOOT_GRAPHEME_CLUSTERING)
        if (grapheme_clustering) {
            /* Check if we're on a grapheme cluster break */
            if (utf8proc_grapheme_break_stateful(
                    last, wc, &term->vt.grapheme_state))
            {
                term_reset_grapheme_state(term);
                goto out;
            }
        }
#endif

        int base_width = c32width(base);
        if (base_width > 0) {
            term->grid->cursor.point.col = col;
            term->grid->cursor.lcf = false;

            if (composed == NULL) {
                bool base_from_primary;
                bool comb_from_primary;
                bool pre_from_primary;

                char32_t precomposed = fcft_precompose(
                    term->fonts[0], base, wc, &base_from_primary,
                    &comb_from_primary, &pre_from_primary);

                int precomposed_width = c32width(precomposed);

                /*
                 * Only use the pre-composed character if:
                 *
                 *  1. we *have* a pre-composed character
                 *  2. the width matches the base characters width
                 *  3. it's in the primary font, OR one of the base or
                 *     combining characters are *not* from the primary
                 *     font
                 */

                if (precomposed != (char32_t)-1 &&
                    precomposed_width == base_width &&
                    (pre_from_primary ||
                     !base_from_primary ||
                     !comb_from_primary))
                {
                    wc = precomposed;
                    width = precomposed_width;
                    term_reset_grapheme_state(term);
                    goto out;
                }
            }

            size_t wanted_count = composed != NULL ? composed->count + 1 : 2;
            if (wanted_count > 255) {
                xassert(composed != NULL);

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
                LOG_WARN("combining character overflow:");
                LOG_WARN("  base: 0x%04x", composed->chars[0]);
                for (size_t i = 1; i < composed->count; i++)
                    LOG_WARN("    cc: 0x%04x", composed->chars[i]);
                LOG_ERR("   new: 0x%04x", wc);
#endif
                /* This is going to break anyway... */
                wanted_count--;
            }

            xassert(wanted_count <= 255);

            size_t collision_count = 0;

            /* Look for existing combining chain */
            while (true) {
                if (unlikely(collision_count > 128)) {
                    static bool have_warned = false;
                    if (!have_warned) {
                        have_warned = true;
                        LOG_WARN("ignoring composed character: "
                                 "too many collisions in hash table");
                    }
                    return;
                }

                const struct composed *cc = composed_lookup(term->composed, key);
                if (cc == NULL)
                    break;

                /*
                 * We may have a key collisison, so need to check that
                 * it’s a true match. If not, bump the key and try
                 * again.
                 */

                xassert(key == cc->key);
                if (cc->chars[0] != base ||
                    cc->count != wanted_count ||
                    cc->chars[wanted_count - 1] != wc)
                {
#if 0
                    LOG_WARN("COLLISION: base: %04x/%04x, count: %d/%zu, last: %04x/%04x",
                             cc->chars[0], base, cc->count, wanted_count, cc->chars[wanted_count - 1], wc);
#endif
                    key++;
                    collision_count++;
                    continue;
                }

                bool match = composed != NULL
                    ? memcmp(&cc->chars[1], &composed->chars[1],
                             (wanted_count - 2) * sizeof(cc->chars[0])) == 0
                    : true;

                if (!match) {
                    key++;
                    collision_count++;
                    continue;
                }

                wc = CELL_COMB_CHARS_LO + cc->key;
                width = cc->width;
                goto out;
            }

            if (unlikely(term->composed_count >=
                         (CELL_COMB_CHARS_HI - CELL_COMB_CHARS_LO)))
            {
                /* We reached our maximum number of allowed composed
                 * character chains. Fall through here and print the
                 * current zero-width character to the current cell */
                LOG_WARN("maximum number of composed characters reached");
                term_reset_grapheme_state(term);
                goto out;
            }

            /* Allocate new chain */
            struct composed *new_cc = xmalloc(sizeof(*new_cc));
            new_cc->chars = xmalloc(wanted_count * sizeof(new_cc->chars[0]));
            new_cc->key = key;
            new_cc->count = wanted_count;
            new_cc->chars[0] = base;
            new_cc->chars[wanted_count - 1] = wc;

            if (composed != NULL) {
                memcpy(&new_cc->chars[1], &composed->chars[1],
                       (wanted_count - 2) * sizeof(new_cc->chars[0]));
            }

            const int grapheme_width =
                composed != NULL ? composed->width : base_width;

            switch (term->conf->tweak.grapheme_width_method) {
            case GRAPHEME_WIDTH_MAX:
                new_cc->width = max(grapheme_width, width);
                break;

            case GRAPHEME_WIDTH_DOUBLE:
                if (unlikely(wc == 0xfe0f))
                    width = 2;
                new_cc->width = min(grapheme_width + width, 2);
                break;

            case GRAPHEME_WIDTH_WCSWIDTH:
                new_cc->width = grapheme_width + width;
                break;
            }

            term->composed_count++;
            composed_insert(&term->composed, new_cc);

            wc = CELL_COMB_CHARS_LO + key;
            width = new_cc->width;

            xassert(wc >= CELL_COMB_CHARS_LO);
            xassert(wc <= CELL_COMB_CHARS_HI);
            goto out;
        }
    } else
        term_reset_grapheme_state(term);


out:
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

IGNORE_WARNING("-Wpedantic")

static enum state
anywhere(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x18:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1a:                                           action_execute(term, data);                                       return STATE_GROUND;
    case 0x1b:                                                                            action_clear(term);              return STATE_ESCAPE;

    /* 8-bit C1 control characters (not supported) */
    case 0x80 ... 0x9f:                                                                                                    return STATE_GROUND;
    }

    return term->vt.state;
}

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
    }

    return anywhere(term, data);
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
    }

    return anywhere(term, data);
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
    }

    return anywhere(term, data);
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
    }

    return anywhere(term, data);
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
    }

    return anywhere(term, data);
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
    }

    return anywhere(term, data);
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
    }

    return anywhere(term, data);
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
    }

    return anywhere(term, data);
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
    }

    return anywhere(term, data);
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
    }

    return anywhere(term, data);
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
    }

    return anywhere(term, data);
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

    /* 8-bit C1 control characters (not supported) */
    case 0x80 ... 0x9f: action_unhook(term, data);                                                                         return STATE_GROUND;

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
    }

    return anywhere(term, data);
}

static enum state
state_utf8_21_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_22(term, data);                                       return STATE_GROUND;
    default:                                                                                                               return STATE_GROUND;
    }
}

static enum state
state_utf8_31_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_32(term, data);                                       return STATE_UTF8_32;
    default:                                                                                                               return STATE_GROUND;
    }
}

static enum state
state_utf8_32_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_33(term, data);                                       return STATE_GROUND;
    default:                                                                                                               return STATE_GROUND;
    }
}

static enum state
state_utf8_41_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_42(term, data);                                       return STATE_UTF8_42;
    default:                                                                                                               return STATE_GROUND;
    }
}

static enum state
state_utf8_42_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_43(term, data);                                       return STATE_UTF8_43;
    default:                                                                                                               return STATE_GROUND;
    }
}

static enum state
state_utf8_43_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                             current                          enter                            new state */
    case 0x80 ... 0xbf:                                  action_utf8_44(term, data);                                       return STATE_GROUND;
    default:                                                                                                               return STATE_GROUND;
    }
}

UNIGNORE_WARNINGS

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

        term->vt.state = current_state;
    }
}
