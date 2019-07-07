#include "osc.h"
#include <ctype.h>
#include "terminal.h"

#define LOG_MODULE "osc"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "render.h"

void
osc_dispatch(struct terminal *term)
{
    int param = 0;
    int data_ofs = 0;

    for (size_t i = 0; i < term->vt.osc.idx; i++) {
        int c = term->vt.osc.data[i];

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

    switch (param) {
    case 0: render_set_title(term, string); break;  /* icon + title */
    case 1: break;                                             /* icon */
    case 2: render_set_title(term, string); break;  /* title */

    case 104: /* Reset Color Number 'c' */
    case 105: /* Reset Special Color Number 'c' */
        break;

    default:
        LOG_ERR("unimplemented: OSC: %.*s",
                (int)term->vt.osc.idx, term->vt.osc.data);
        abort();
        break;
    }
}
