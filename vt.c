#include "vt.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#define LOG_MODULE "vt"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "csi.h"
#include "grid.h"
#include "osc.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

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

    STATE_UTF8_COLLECT_1,
    STATE_UTF8_COLLECT_2,
    STATE_UTF8_COLLECT_3,
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

    [STATE_UTF8_COLLECT_1] = "UTF8 collect (1 left)",
    [STATE_UTF8_COLLECT_2] = "UTF8 collect (2 left)",
    [STATE_UTF8_COLLECT_3] = "UTF8 collect (3 left)",
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

static inline void
pre_print(struct terminal *term)
{
    if (likely(!term->cursor.lcf))
        return;
    if (unlikely(!term->auto_margin))
        return;

    if (term->cursor.point.row == term->scroll_region.end - 1) {
        term_scroll(term, 1);
        term_cursor_to(term, term->cursor.point.row, 0);
    } else
        term_cursor_to(term, min(term->cursor.point.row + 1, term->rows - 1), 0);
}

static inline void
post_print(struct terminal *term)
{
    if (term->cursor.point.col < term->cols - 1)
        term_cursor_right(term, 1);
    else
        term->cursor.lcf = true;
}

static inline void
print_insert(struct terminal *term, int width)
{
    assert(width > 0);
    if (unlikely(term->insert_mode)) {
        struct row *row = term->grid->cur_row;
        const size_t move_count = max(0, term->cols - term->cursor.point.col - width);

        memmove(
            &row->cells[term->cursor.point.col + width],
            &row->cells[term->cursor.point.col],
            move_count * sizeof(struct cell));

        /* Mark moved cells as dirty */
        for (size_t i = term->cursor.point.col + width; i < term->cols; i++)
            row->cells[i].attrs.clean = 0;
    }
}

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
    term->vt.utf8.idx = 0;
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

    case '\n':
        /* LF - line feed */
        term_linefeed(term);
        break;

    case '\r':
        /* FF - form feed */
        term_cursor_left(term, term->cursor.point.col);
        break;

    case '\b':
        /* backspace */
        term_cursor_left(term, 1);
        break;

    case '\x07':
        /* BEL */
        // LOG_INFO("BELL");
        // term_flash(term, 50);
        break;

    case '\x09': {
        /* HT - horizontal tab */
        int new_col = term->cols - 1;
        tll_foreach(term->tab_stops, it) {
            if (it->item > term->cursor.point.col) {
                new_col = it->item;
                break;
            }
        }
        assert(new_col >= term->cursor.point.col);
        term_cursor_right(term, new_col - term->cursor.point.col);
        break;
    }

    case '\x0b':
        /* VT - vertical tab */
        term_cursor_down(term, 1);
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
    pre_print(term);

    struct row *row = term->grid->cur_row;
    struct cell *cell = &row->cells[term->cursor.point.col];

    row->dirty = true;
    cell->attrs.clean = 0;

    print_insert(term, 1);

    /* 0x60 - 0x7e */
    static const wchar_t vt100_0[] = {
        L'◆', L'▒', L'␉', L'␌', L'␍', L'␊', L'°', L'±', /* ` - g */
        L'␤', L'␋', L'┘', L'┐', L'┌', L'└', L'┼', L'⎺', /* h - o */
        L'⎻', L'─', L'⎼', L'⎽', L'├', L'┤', L'┴', L'┬', /* p - w */
        L'│', L'≤', L'≥', L'π', L'≠', L'£', L'·',       /* x - ~ */
    };

    if (unlikely(term->charsets.set[term->charsets.selected] == CHARSET_GRAPHIC) &&
        c >= 0x60 && c <= 0x7e)
    {
        cell->wc = vt100_0[c - 0x60];
    } else {
        // LOG_DBG("print: ASCII: %c (0x%04x)", c, c);
        cell->wc = c;
    }

    cell->attrs = term->vt.attrs;
    post_print(term);
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

    if (c == ';') {
        struct vt_param *param = &term->vt.params.v[term->vt.params.idx++];
        param->value = 0;
        param->sub.idx = 0;
    } else if (c == ':') {
        struct vt_param *param = &term->vt.params.v[term->vt.params.idx - 1];
        param->sub.value[param->sub.idx++] = 0;
    } else {
        assert(term->vt.params.idx >= 0);
        struct vt_param *param = &term->vt.params.v[term->vt.params.idx - 1];

        unsigned *value = param->sub.idx > 0
            ? &param->sub.value[param->sub.idx - 1]
            : &param->value;

        *value *= 10;
        *value += c - '0';
    }
}

static void
action_collect(struct terminal *term, uint8_t c)
{
    LOG_DBG("collect");
    if (term->vt.private[0] == 0)
        term->vt.private[0] = c;
    else if (term->vt.private[1] == 0)
        term->vt.private[1] = c;
    else
        LOG_DBG("only two private/intermediate characters supported");
}

static void
action_esc_dispatch(struct terminal *term, uint8_t final)
{
    LOG_DBG("ESC: %s", esc_as_string(term, final));

    switch (term->vt.private[0]) {
    case 0:
        switch (final) {
        case '7':
            term->saved_cursor = term->cursor;
            term->vt.saved_attrs = term->vt.attrs;
            term->saved_charsets = term->charsets;
            break;

        case '8':
            term_restore_cursor(term);
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
            term_linefeed(term);
            term_cursor_left(term, term->cursor.point.col);
            break;

        case 'H':
            tll_foreach(term->tab_stops, it) {
                if (it->item >= term->cursor.point.col) {
                    tll_insert_before(term->tab_stops, it, term->cursor.point.col);
                    break;
                }
            }

            tll_push_back(term->tab_stops, term->cursor.point.col);
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
}

static void
action_unhook(struct terminal *term, uint8_t c)
{
}

static void
action_put(struct terminal *term, uint8_t c)
{
}

static void
action_utf8_2_entry(struct terminal *term, uint8_t c)
{
    term->vt.utf8.idx = 0;
    term->vt.utf8.left = 2;
    term->vt.utf8.data[term->vt.utf8.idx++] = c;
    term->vt.utf8.left--;
}

static void
action_utf8_3_entry(struct terminal *term, uint8_t c)
{
    term->vt.utf8.idx = 0;
    term->vt.utf8.left = 3;
    term->vt.utf8.data[term->vt.utf8.idx++] = c;
    term->vt.utf8.left--;
}

static void
action_utf8_4_entry(struct terminal *term, uint8_t c)
{
    term->vt.utf8.idx = 0;
    term->vt.utf8.left = 4;
    term->vt.utf8.data[term->vt.utf8.idx++] = c;
    term->vt.utf8.left--;
}

static void
action_utf8_print(struct terminal *term, uint8_t c)
{
    pre_print(term);

    struct row *row = term->grid->cur_row;
    struct cell *cell = &row->cells[term->cursor.point.col];

    /* Convert to wchar */
    mbstate_t ps = {0};
    wchar_t wc;
    if (mbrtowc(&wc, (const char *)term->vt.utf8.data, term->vt.utf8.idx, &ps) < 0)
        wc = 0;

    int width = wcwidth(wc);
    if (width > 0)
        print_insert(term, width);

    row->dirty = true;
    cell->wc = wc;
    cell->attrs.clean = 0;
    cell->attrs = term->vt.attrs;

    /* Reset VT utf8 state */
    term->vt.utf8.idx = 0;

    if (width <= 0) {
        /* Skip post_print() below - i.e. don't advance cursor */
        return;
    }

    /* Advance cursor the 'additional' columns (last step is done
     * by post_print()) */
    for (int i = 1; i < width && term->cursor.point.col < term->cols - 1; i++) {
        term_cursor_right(term, 1);

        assert(term->cursor.point.col < term->cols);
        struct cell *cell = &row->cells[term->cursor.point.col];
        cell->wc = 0;
        cell->attrs.clean = 0;
    }

    post_print(term);
}

static enum state
state_ground_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                          action_execute(term, data);                                                       return STATE_GROUND;

    case 0x20 ... 0x7f:                                          action_print(term, data);                                                         return STATE_GROUND;

    case 0xc0 ... 0xdf:                                          action_utf8_2_entry(term, data);                                                  return STATE_UTF8_COLLECT_1;
    case 0xe0 ... 0xef:                                          action_utf8_3_entry(term, data);                                                  return STATE_UTF8_COLLECT_2;
    case 0xf0 ... 0xf7:                                          action_utf8_4_entry(term, data);                                                  return STATE_UTF8_COLLECT_3;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_GROUND;
    }
}

static enum state
state_escape_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                          action_execute(term, data);                                                       return STATE_ESCAPE;

    case 0x20 ... 0x2f:                                          action_collect(term, data);                                                       return STATE_ESCAPE_INTERMEDIATE;
    case 0x30 ... 0x4f:                                          action_esc_dispatch(term, data);                                                  return STATE_GROUND;
    case 0x50:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x51 ... 0x57:                                          action_esc_dispatch(term, data);                                                  return STATE_GROUND;
    case 0x58:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x59:                                                   action_esc_dispatch(term, data);                                                  return STATE_GROUND;
    case 0x5a:                                                   action_esc_dispatch(term, data);                                                  return STATE_GROUND;
    case 0x5b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x5c:                                                   action_esc_dispatch(term, data);                                                  return STATE_GROUND;
    case 0x5d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x5e ... 0x5f:                                                                                                                            return STATE_SOS_PM_APC_STRING;
    case 0x60 ... 0x7e:                                          action_esc_dispatch(term, data);                                                  return STATE_GROUND;
    case 0x7f:                                                   action_ignore(term);                                                              return STATE_ESCAPE;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_ESCAPE;
    }
}

static enum state
state_escape_intermediate_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                          action_execute(term, data);                                                       return STATE_ESCAPE_INTERMEDIATE;

    case 0x20 ... 0x2f:                                          action_collect(term, data);                                                       return STATE_ESCAPE_INTERMEDIATE;
    case 0x30 ... 0x7e:                                          action_esc_dispatch(term, data);                                                  return STATE_GROUND;
    case 0x7f:                                                   action_ignore(term);                                                              return STATE_ESCAPE_INTERMEDIATE;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_ESCAPE_INTERMEDIATE;
    }
}

static enum state
state_csi_entry_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                          action_execute(term, data);                                                       return STATE_CSI_ENTRY;

    case 0x20 ... 0x2f:                                          action_collect(term, data);                                                       return STATE_CSI_INTERMEDIATE;
    case 0x30 ... 0x39:                                          action_param(term, data);                                                         return STATE_CSI_PARAM;
    case 0x3a ... 0x3b:                                                                                                                            return STATE_CSI_PARAM;
    case 0x3c ... 0x3f:                                          action_collect(term, data);                                                       return STATE_CSI_PARAM;
    case 0x40 ... 0x7e:                                          action_csi_dispatch(term, data);                                                  return STATE_GROUND;
    case 0x7f:                                                   action_ignore(term);                                                              return STATE_CSI_ENTRY;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_CSI_ENTRY;
    }
}

static enum state
state_csi_param_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                          action_execute(term, data);                                                       return STATE_CSI_PARAM;

    case 0x20 ... 0x2f:                                          action_collect(term, data);                                                       return STATE_CSI_INTERMEDIATE;

    case 0x30 ... 0x39:
    case 0x3a ... 0x3b:                                          action_param(term, data);                                                         return STATE_CSI_PARAM;

    case 0x3c ... 0x3f:                                                                                                                            return STATE_CSI_IGNORE;
    case 0x40 ... 0x7e:                                          action_csi_dispatch(term, data);                                                  return STATE_GROUND;
    case 0x7f:                                                   action_ignore(term);                                                              return STATE_CSI_PARAM;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_CSI_PARAM;
    }
}

static enum state
state_csi_intermediate_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                          action_execute(term, data);                                                       return STATE_CSI_INTERMEDIATE;

    case 0x20 ... 0x2f:                                          action_collect(term, data);                                                       return STATE_CSI_INTERMEDIATE;
    case 0x30 ... 0x3f:                                                                                                                            return STATE_CSI_IGNORE;
    case 0x40 ... 0x7e:                                          action_csi_dispatch(term, data);                                                  return STATE_GROUND;
    case 0x7f:                                                   action_ignore(term);                                                              return STATE_CSI_INTERMEDIATE;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_CSI_INTERMEDIATE;
    }
}

static enum state
state_csi_ignore_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                         action_execute(term, data);                                                        return STATE_CSI_IGNORE;

    case 0x20 ... 0x3f:                                         action_ignore(term);                                                               return STATE_CSI_IGNORE;
    case 0x40 ... 0x7e:                                                                                                                            return STATE_GROUND;
    case 0x7f:                                                  action_ignore(term);                                                               return STATE_CSI_IGNORE;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_CSI_IGNORE;
    }
}

static enum state
state_osc_string_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */

    /* Note: original was 20-7f, but I changed to 20-ff to include utf-8. Don't forget to add EXECUTE to 8-bit C1 if we implement that. */
    default:                                                     action_osc_put(term, data);                                                       return STATE_OSC_STRING;

    case 0x07:          action_osc_end(term, data);                                                                                                return STATE_GROUND;

    case 0x00 ... 0x06:
    case 0x08 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                          action_ignore(term);                                                              return STATE_OSC_STRING;


    case 0x18:
    case 0x1a:          action_osc_end(term, data);              action_execute(term, data);                                                       return STATE_GROUND;

    case 0x1b:          action_osc_end(term, data);              action_clear(term);                                                               return STATE_ESCAPE;
    }
}

static enum state
state_dcs_entry_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                          action_ignore(term);                                                              return STATE_DCS_ENTRY;

    case 0x20 ... 0x2f:                                          action_collect(term, data);                                                       return STATE_DCS_INTERMEDIATE;
    case 0x30 ... 0x39:                                          action_param(term, data);                                                         return STATE_DCS_PARAM;
    case 0x3a:                                                                                                                                     return STATE_DCS_IGNORE;
    case 0x3b:                                                   action_param(term, data);                                                         return STATE_DCS_PARAM;
    case 0x3c ... 0x3f:                                          action_collect(term, data);                                                       return STATE_DCS_PARAM;
    case 0x40 ... 0x7e:                                                                                   action_hook(term, data);                 return STATE_DCS_PASSTHROUGH;
    case 0x7f:                                                   action_ignore(term);                                                              return STATE_DCS_ENTRY;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_DCS_ENTRY;
    }
}

static enum state
state_dcs_param_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                          action_ignore(term);                                                              return STATE_DCS_PARAM;

    case 0x20 ... 0x2f:                                          action_collect(term, data);                                                       return STATE_DCS_INTERMEDIATE;
    case 0x30 ... 0x39:                                          action_param(term, data);                                                         return STATE_DCS_PARAM;
    case 0x3a:                                                                                                                                     return STATE_DCS_IGNORE;
    case 0x3b:                                                   action_param(term, data);                                                         return STATE_DCS_PARAM;
    case 0x3c ... 0x3f:                                                                                                                            return STATE_DCS_IGNORE;
    case 0x40 ... 0x7e:                                                                                   action_hook(term, data);                 return STATE_DCS_PASSTHROUGH;
    case 0x7f:                                                   action_ignore(term);                                                              return STATE_DCS_PARAM;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_DCS_PARAM;
    }
}

static enum state
state_dcs_intermediate_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:                                          action_ignore(term);                                                              return STATE_DCS_INTERMEDIATE;

    case 0x20 ... 0x2f:                                          action_collect(term, data);                                                       return STATE_DCS_INTERMEDIATE;
    case 0x30 ... 0x3f:                                                                                                                            return STATE_DCS_IGNORE;
    case 0x40 ... 0x7e:                                                                                   action_hook(term, data);                 return STATE_DCS_PASSTHROUGH;
    case 0x7f:                                                   action_ignore(term);                                                              return STATE_DCS_INTERMEDIATE;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_DCS_INTERMEDIATE;
    }
}

static enum state
state_dcs_ignore_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x1f:
    case 0x20 ... 0x7f:                                          action_ignore(term);                                                              return STATE_DCS_IGNORE;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_DCS_IGNORE;
    }
}

static enum state
state_dcs_passthrough_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x7e:                                          action_put(term, data);                                                           return STATE_DCS_PASSTHROUGH;

    case 0x7f:                                                   action_ignore(term);                                                              return STATE_DCS_PASSTHROUGH;

    /* Anywhere */
    case 0x18:          action_unhook(term, data);               action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:          action_unhook(term, data);               action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:          action_unhook(term, data);                                                        action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f: action_unhook(term, data);               action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:          action_unhook(term, data);                                                        action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97: action_unhook(term, data);               action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:          action_unhook(term, data);                                                                                                 return STATE_SOS_PM_APC_STRING;
    case 0x99:          action_unhook(term, data);               action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:          action_unhook(term, data);               action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:          action_unhook(term, data);                                                        action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:          action_unhook(term, data);                                                                                                 return STATE_GROUND;
    case 0x9d:          action_unhook(term, data);                                                        action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f: action_unhook(term, data);                                                                                                 return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_DCS_PASSTHROUGH;
    }
}

static enum state
state_sos_pm_apc_string_switch(struct terminal *term, uint8_t data)
{
    switch (data) {
        /*              exit                                     current                                  enter                                    new state */
    case 0x00 ... 0x17:
    case 0x19:
    case 0x1c ... 0x7f:                                          action_ignore(term);                                                              return STATE_SOS_PM_APC_STRING;

    /* Anywhere */
    case 0x18:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x1b:                                                                                            action_clear(term);                      return STATE_ESCAPE;
    case 0x80 ... 0x8f:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x90:                                                                                            action_clear(term);                      return STATE_DCS_ENTRY;
    case 0x91 ... 0x97:                                          action_execute(term, data);                                                       return STATE_GROUND;
    case 0x98:                                                                                                                                     return STATE_SOS_PM_APC_STRING;
    case 0x99:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9a:                                                   action_execute(term, data);                                                       return STATE_GROUND;
    case 0x9b:                                                                                            action_clear(term);                      return STATE_CSI_ENTRY;
    case 0x9c:                                                                                                                                     return STATE_GROUND;
    case 0x9d:                                                                                            action_osc_start(term, data);            return STATE_OSC_STRING;
    case 0x9e ... 0x9f:                                                                                                                            return STATE_SOS_PM_APC_STRING;

    default:                                                                                                                                       return STATE_SOS_PM_APC_STRING;
    }
}

static enum state
state_utf8_collect_1_switch(struct terminal *term, uint8_t data)
{
    term->vt.utf8.data[term->vt.utf8.idx++] = data;
    term->vt.utf8.left--;

    assert(term->vt.utf8.left == 0);
    action_utf8_print(term, data);
    return STATE_GROUND;
}

static enum state
state_utf8_collect_2_switch(struct terminal *term, uint8_t data)
{
    term->vt.utf8.data[term->vt.utf8.idx++] = data;
    term->vt.utf8.left--;

    assert(term->vt.utf8.left == 1);
    return STATE_UTF8_COLLECT_1;
}

static enum state
state_utf8_collect_3_switch(struct terminal *term, uint8_t data)
{
    term->vt.utf8.data[term->vt.utf8.idx++] = data;
    term->vt.utf8.left--;

    assert(term->vt.utf8.left == 2);
    return STATE_UTF8_COLLECT_2;
}

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

        case STATE_UTF8_COLLECT_1:      current_state = state_utf8_collect_1_switch(term, *p); break;
        case STATE_UTF8_COLLECT_2:      current_state = state_utf8_collect_2_switch(term, *p); break;
        case STATE_UTF8_COLLECT_3:      current_state = state_utf8_collect_3_switch(term, *p); break;
        }
    }


    term->vt.state = current_state;
}
