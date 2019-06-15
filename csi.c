#include "csi.h"

#define LOG_MODULE "csi"
#define LOG_ENABLE_DBG 1
#include "log.h"

static bool
csi_sgr(struct terminal *term)
{
    for (size_t i = 0; i < term->vt.params.idx; i++) {
        switch (term->vt.params.v[i].value) {
        case 0:
            term->vt.bold = false;
            term->vt.dim = false;
            term->vt.italic = false;
            term->vt.underline = false;
            term->vt.strikethrough = false;
            term->vt.blink = false;
            term->vt.conceal = false;
            term->vt.reverse = false;
            term->vt.foreground = term->grid.foreground;
            term->vt.background = term->grid.background;
            break;

        case 1: term->vt.bold = true; break;
        case 2: term->vt.dim = true; break;
        case 3: term->vt.italic = true; break;
        case 4: term->vt.underline = true; break;
        case 5: term->vt.blink = true; break;
        case 6: term->vt.blink = true; break;
        case 7: term->vt.reverse = true; break;
        case 8: term->vt.conceal = true; break;
        case 9: term->vt.strikethrough = true; break;

        case 22: term->vt.bold = term->vt.dim = false; break;
        case 23: term->vt.italic = false; break;
        case 24: term->vt.underline = false; break;
        case 25: term->vt.blink = false; break;
        case 27: term->vt.reverse = false; break;
        case 28: term->vt.conceal = false; break;
        case 29: term->vt.strikethrough = false; break;

        case 30: term->vt.foreground = 0x000000ff; break;
        case 31: term->vt.foreground = 0xff0000ff; break;
        case 32: term->vt.foreground = 0x00ff00ff; break;
        case 33: term->vt.foreground = 0xf0f000ff; break;
        case 34: term->vt.foreground = 0x0000ffff; break;
        case 35: term->vt.foreground = 0xf000f0ff; break;
        case 36: term->vt.foreground = 0x00f0f0ff; break;
        case 37: term->vt.foreground = 0xffffffff; break;
        case 39: term->vt.foreground = term->grid.foreground; break;

        case 40: term->vt.background = 0x000000ff; break;
        case 41: term->vt.background = 0xff0000ff; break;
        case 42: term->vt.background = 0x00ff00ff; break;
        case 43: term->vt.background = 0xf0f000ff; break;
        case 44: term->vt.background = 0x0000ffff; break;
        case 45: term->vt.background = 0xf000f0ff; break;
        case 46: term->vt.background = 0x00f0f0ff; break;
        case 47: term->vt.background = 0xffffffff; break;
        case 49: term->vt.background = term->grid.background; break;

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
    LOG_DBG("CSI: %zu paramaters, final = %c", term->vt.params.idx, final);
    for (size_t i = 0; i < term->vt.params.idx; i++) {
        LOG_DBG("  #%zu: %u", i, term->vt.params.v[i].value);
        for (size_t j = 0; j < term->vt.params.v[i].sub.idx; j++)
            LOG_DBG("    #%zu: %u", j, term->vt.params.v[i].sub.value[j]);
    }

    if (final == 'm' && term->vt.intermediates.idx == 0) {
        return csi_sgr(term);
    }

    return true;
}
