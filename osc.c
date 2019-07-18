#include "osc.h"

#include <string.h>
#include <ctype.h>

#define LOG_MODULE "osc"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "render.h"
#include "terminal.h"
#include "vt.h"

static void
osc_query(struct terminal *term, unsigned param)
{
    switch (param) {
    case 10:
    case 11: {
        char reply[16];
        snprintf(
            reply, sizeof(reply), "\033]%u;%06x\x07",
            param,
            (param == 10 ? term->foreground : term->background) & 0xffffff);
        vt_to_slave(term, reply, strlen(reply));
        break;
    }

    default:
        LOG_ERR("unimplemented: OSC query: %.*s",
                (int)term->vt.osc.idx, term->vt.osc.data);
        abort();
        break;
    }
}

void
osc_dispatch(struct terminal *term)
{
    unsigned param = 0;
    int data_ofs = 0;

    for (size_t i = 0; i < term->vt.osc.idx; i++) {
        char c = term->vt.osc.data[i];

        if (c == ';') {
            data_ofs = i + 1;
            break;
        }

        if (!isdigit(c)) {
            LOG_ERR("OSC: invalid parameter: %.*s",
                    (int)term->vt.osc.idx, term->vt.osc.data);
            abort();
        }

        param *= 10;
        param += c - '0';
    }
    LOG_DBG("OCS: %.*s (param = %d)",
            (int)term->vt.osc.idx, term->vt.osc.data, param);

    const char *string = (const char *)&term->vt.osc.data[data_ofs];

    if (strlen(string) == 1 && string[0] == '?') {
        osc_query(term, param);
        return;
    }

    switch (param) {
    case 0: render_set_title(term, string); break;  /* icon + title */
    case 1: break;                                             /* icon */
    case 2: render_set_title(term, string); break;  /* title */

    case 104: /* Reset Color Number 'c' */
    case 105: /* Reset Special Color Number 'c' */
    case 112: /* Reset text cursor color */
        break;

    default:
        LOG_ERR("unimplemented: OSC: %.*s",
                (int)term->vt.osc.idx, term->vt.osc.data);
        abort();
        break;
    }
}
