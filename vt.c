#include "vt.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define LOG_MODULE "vt"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "csi.h"
#include "osc.h"
#include "grid.h"

/* https://vt100.net/emu/dec_ansi_parser */

enum state {
    STATE_SAME,       /* For state_transition */

    STATE_ANYWHERE,
    STATE_ESCAPE,
    STATE_GROUND,
    STATE_CSIENTRY,
    STATE_CSIPARAM,
    STATE_OSCSTRING,

    STATE_UTF8,
};

enum action {
    ACTION_NONE,      /* For state_transition */

    ACTION_IGNORE,
    ACTION_CLEAR,
    ACTION_EXECUTE,
    ACTION_PRINT,
    ACTION_PARAM,
    ACTION_COLLECT,
    ACTION_CSIDISPATCH,
    ACTION_OSCSTART,
    ACTION_OSCEND,
    ACTION_OSCPUT,

    ACTION_UTF8,
};

static const char *const state_names[] = {
    [STATE_SAME] = "no change",
    [STATE_ANYWHERE] = "anywhere",
    [STATE_ESCAPE] = "escape",
    [STATE_GROUND] = "ground",
    [STATE_CSIENTRY] = "CSI entry",
    [STATE_CSIPARAM] = "CSI param",
    [STATE_OSCSTRING] = "OSC string",

    [STATE_UTF8] = "UTF-8",
};

static const char *const action_names[] __attribute__((unused)) = {
    [ACTION_NONE] = "no action",
    [ACTION_IGNORE] = "ignore",
    [ACTION_CLEAR] = "clear",
    [ACTION_EXECUTE] = "execute",
    [ACTION_PRINT] = "print",
    [ACTION_PARAM] = "param",
    [ACTION_COLLECT] = "collect",
    [ACTION_CSIDISPATCH] = "CSI dispatch",
    [ACTION_OSCSTART] = "OSC start",
    [ACTION_OSCEND] = "OSC end",
    [ACTION_OSCPUT] = "OSC put",

    [ACTION_UTF8] = "begin UTF-8",
};

struct state_transition {
    enum action action;
    enum state state;
};

static const struct state_transition state_anywhere[256] = {
    [0x1b] = {.state = STATE_ESCAPE},
};

static const struct state_transition state_ground[256] = {
    [0x00 ... 0x17] = {.action = ACTION_EXECUTE},
    [0x20 ... 0x7f] = {.action = ACTION_PRINT},
    [0xc2 ... 0xdf] = {.action = ACTION_UTF8, .state = STATE_UTF8}, /* 2 chars */
    [0xe0 ... 0xef] = {.action = ACTION_UTF8, .state = STATE_UTF8}, /* 3 chars */
    [0xf0 ... 0xf4] = {.action = ACTION_UTF8, .state = STATE_UTF8}, /* 4 chars */
};

static const struct state_transition state_escape[256] = {
    [0x5b] = {.state = STATE_CSIENTRY},
    [0x5d] = {.state = STATE_OSCSTRING},
};

static const struct state_transition state_csientry[256] = {
    [0x30 ... 0x39] = {.action = ACTION_PARAM, .state = STATE_CSIPARAM},
    [0x3b] = {.action = ACTION_PARAM, .state = STATE_CSIPARAM},
    [0x3c ... 0x3f] = {.action = ACTION_COLLECT, .state = STATE_CSIPARAM},
    [0x40 ... 0x7e] = {.action = ACTION_CSIDISPATCH, .state = STATE_GROUND},
    [0x6d] = {.action = ACTION_CSIDISPATCH, .state = STATE_GROUND},
};

static const struct state_transition state_csiparam[256] = {
    [0x30 ... 0x39] = {.action = ACTION_PARAM},
    [0x3b] = {.action = ACTION_PARAM},
    [0x40 ... 0x7e] = {.action = ACTION_CSIDISPATCH, .state = STATE_GROUND},
    [0x6d] = {.action = ACTION_CSIDISPATCH, .state = STATE_GROUND},
};

static const struct state_transition state_ocsstring[256] = {
    [0x07] = {.state = STATE_GROUND},  /* Not in diagram */
    [0x20 ... 0x7f] = {.action = ACTION_OSCPUT},
};

static const struct state_transition* states[] = {
    [STATE_ANYWHERE] = state_anywhere,
    [STATE_ESCAPE] = state_escape,
    [STATE_GROUND] = state_ground,
    [STATE_CSIENTRY] = state_csientry,
    [STATE_CSIPARAM] = state_csiparam,
    [STATE_OSCSTRING] = state_ocsstring,
};

static const enum action entry_actions[] = {
    [STATE_SAME] = ACTION_NONE,
    [STATE_ANYWHERE] = ACTION_NONE,
    [STATE_ESCAPE] = ACTION_NONE,
    [STATE_GROUND] = ACTION_NONE,
    [STATE_CSIENTRY] = ACTION_CLEAR,
    [STATE_CSIPARAM] = ACTION_NONE,
    [STATE_OSCSTRING] = ACTION_OSCSTART,
    [STATE_UTF8] = ACTION_NONE,
};

static const enum action exit_actions[] = {
    [STATE_SAME] = ACTION_NONE,
    [STATE_ANYWHERE] = ACTION_NONE,
    [STATE_ESCAPE] = ACTION_NONE,
    [STATE_GROUND] = ACTION_NONE,
    [STATE_CSIENTRY] = ACTION_NONE,
    [STATE_CSIPARAM] = ACTION_NONE,
    [STATE_OSCSTRING] = ACTION_OSCEND,
    [STATE_UTF8] = ACTION_NONE,
};

static bool
action(struct terminal *term, enum action action, uint8_t c)
{
    switch (action) {
    case ACTION_NONE:
        break;

    case ACTION_IGNORE:
        break;

    case ACTION_EXECUTE:
        LOG_DBG("execute: 0x%02x", c);
        switch (c) {
        case '\r':
            grid_cursor_left(&term->grid, term->grid.cursor.col);
            break;

        case '\b':
            grid_cursor_left(&term->grid, 1);
            break;
        }

        return true;

    case ACTION_CLEAR:
        memset(&term->vt.params, 0, sizeof(term->vt.params));
        memset(&term->vt.intermediates, 0, sizeof(term->vt.intermediates));
        memset(&term->vt.osc, 0, sizeof(term->vt.osc));
        memset(&term->vt.utf8, 0, sizeof(term->vt.utf8));
        break;

    case ACTION_PRINT: {
        if (term->grid.print_needs_wrap)
            grid_cursor_to(&term->grid, term->grid.cursor.row + 1, 0);

        struct cell *cell = &term->grid.cells[term->grid.linear_cursor];

        cell->dirty = true;

        if (term->vt.utf8.idx > 0) {
            //LOG_DBG("print: UTF8: %.*s", (int)term->vt.utf8.idx, term->vt.utf8.data);
            memcpy(cell->c, term->vt.utf8.data, term->vt.utf8.idx);
            cell->c[term->vt.utf8.idx] = '\0';
        } else {
            //LOG_DBG("print: ASCII: %c", c);
            cell->c[0] = c;
            cell->c[1] = '\0';
        }

        cell->attrs = term->vt.attrs;

        if (term->grid.cursor.col < term->grid.cols - 1)
            grid_cursor_right(&term->grid, 1);
        else
            term->grid.print_needs_wrap = true;

        term->grid.dirty = true;
        break;
    }

    case ACTION_PARAM:{
        if (term->vt.params.idx == 0)
            term->vt.params.idx = 1;

        if (c == ';') {
            term->vt.params.idx++;
        } else if (c == ':') {
            if (term->vt.params.v[term->vt.params.idx - 1].sub.idx == 0)
                term->vt.params.v[term->vt.params.idx - 1].sub.idx = 1;
        } else {
            if (term->vt.params.v[term->vt.params.idx - 1].sub.idx > 0)
                term->vt.params.v[term->vt.params.idx - 1].sub.value[term->vt.params.v[term->vt.params.idx - 1].sub.idx] *= 10;
            else
                term->vt.params.v[term->vt.params.idx - 1].value *= 10;

            if (term->vt.params.v[term->vt.params.idx - 1].sub.idx > 0)
                term->vt.params.v[term->vt.params.idx - 1].sub.value[term->vt.params.v[term->vt.params.idx - 1].sub.idx] += c - '0';
            else
                term->vt.params.v[term->vt.params.idx - 1].value += c - '0';
        }
        break;
                }

    case ACTION_COLLECT:
        LOG_DBG("collect");
        term->vt.intermediates.data[term->vt.intermediates.idx++] = c;
        break;

    case ACTION_CSIDISPATCH:
        return csi_dispatch(term, c);

    case ACTION_OSCSTART:
        term->vt.osc.idx = 0;
        break;

    case ACTION_OSCPUT:
        term->vt.osc.data[term->vt.osc.idx++] = c;
        break;

    case ACTION_OSCEND:
        return osc_dispatch(term);

    case ACTION_UTF8:
        term->vt.utf8.idx = 0;
        if (c >= 0x2c && c <= 0xdf)
            term->vt.utf8.left = 2;
        else if (c >= 0xe0 && c <= 0xef)
            term->vt.utf8.left = 3;
        else
            term->vt.utf8.left = 4;
        //LOG_DBG("begin UTF-8 (%zu chars)", term->vt.utf8.left);
        term->vt.utf8.data[term->vt.utf8.idx++] = c;
        term->vt.utf8.left--;
        break;
    }

    return true;
}

static bool
process_utf8(struct terminal *term, uint8_t c)
{
    //LOG_DBG("UTF-8: 0x%02x", c);
    term->vt.utf8.data[term->vt.utf8.idx++] = c;
    term->vt.utf8.left--;

    if (term->vt.utf8.left == 0)
        term->vt.state = STATE_GROUND;
    return true;
}

void
vt_from_slave(struct terminal *term, const uint8_t *data, size_t len)
{
    //int cursor = term->grid.cursor;
    for (size_t i = 0; i < len; i++) {
        //LOG_DBG("input: 0x%02x", data[i]);
        enum state current_state = term->vt.state;

        const struct state_transition *transition = &state_anywhere[data[i]];
        if (transition->action == ACTION_NONE && transition->state == STATE_SAME) {
            if (current_state == STATE_UTF8) {
                if (!process_utf8(term, data[i]))
                    abort();
                if (current_state == STATE_UTF8)
                    continue;
                if (!action(term, ACTION_PRINT, 0))
                    abort();
                continue;
            }

            transition = &states[current_state][data[i]];
            if (transition->action == ACTION_NONE && transition->state == STATE_SAME) {
                LOG_ERR("unimplemented transition from %s: 0x%02x",
                        state_names[current_state], data[i]);
                abort();
            }
        }

        if (transition->state != STATE_SAME) {
            enum action exit_action = exit_actions[current_state];
            if (exit_action != ACTION_NONE && !action(term, exit_action, data[i]))
                abort();
        }

        if (!action(term, transition->action, data[i]))
            abort();

        if (transition->state != STATE_SAME) {
            /*
             * LOG_DBG("transition: %s -> %s", state_names[current_state],
             *         state_names[transition->state]);
             */
            term->vt.state = transition->state;

            enum action entry_action = entry_actions[transition->state];
            if (entry_action != ACTION_NONE && !action(term, entry_action, data[i]))
                abort();
        }
    }
}
