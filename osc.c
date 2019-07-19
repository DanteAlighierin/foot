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
        uint32_t color = param == 10 ? term->foreground : term->background;
        uint8_t r = (color >> 16) & 0xff;
        uint8_t g = (color >>  8) & 0xff;
        uint8_t b = (color >>  0) & 0xff;

        /*
         * Reply in XParseColor format
         * E.g. for color 0xdcdccc we reply "\e]10;rgb:dc/dc/cc\e\\"
         */
        char reply[32];
        snprintf(
            reply, sizeof(reply), "\033]%u;rgb:%02x/%02x/%02x\033\\",
            param, r, g, b);

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

bool
osc_ensure_size(struct terminal *term, size_t required_size)
{
    if (required_size <= term->vt.osc.size)
        return true;

    size_t new_size = (required_size + 127) / 128 * 128;
    assert(new_size > 0);

    uint8_t *new_data = realloc(term->vt.osc.data, new_size);
    if (new_data == NULL) {
        LOG_ERRNO("failed to increase size of OSC buffer");
        return false;
    }

    term->vt.osc.data = new_data;
    term->vt.osc.size = new_size;
    return true;
}
