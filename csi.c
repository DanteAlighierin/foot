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
            term->vt.attrs.bold = false;
            term->vt.dim = false;
            term->vt.attrs.italic = false;
            term->vt.attrs.underline = false;
            term->vt.attrs.strikethrough = false;
            term->vt.attrs.blink = false;
            term->vt.attrs.conceal = false;
            term->vt.attrs.reverse = false;
            term->vt.attrs.foreground = term->grid.foreground;
            term->vt.attrs.background = term->grid.background;
            break;

        case 1: term->vt.attrs.bold = true; break;
        case 2: term->vt.dim = true; break;
        case 3: term->vt.attrs.italic = true; break;
        case 4: term->vt.attrs.underline = true; break;
        case 5: term->vt.attrs.blink = true; break;
        case 6: term->vt.attrs.blink = true; break;
        case 7: term->vt.attrs.reverse = true; break;
        case 8: term->vt.attrs.conceal = true; break;
        case 9: term->vt.attrs.strikethrough = true; break;

        case 22: term->vt.attrs.bold = term->vt.dim = false; break;
        case 23: term->vt.attrs.italic = false; break;
        case 24: term->vt.attrs.underline = false; break;
        case 25: term->vt.attrs.blink = false; break;
        case 27: term->vt.attrs.reverse = false; break;
        case 28: term->vt.attrs.conceal = false; break;
        case 29: term->vt.attrs.strikethrough = false; break;

        case 30: term->vt.attrs.foreground = 0x000000ff; break;
        case 31: term->vt.attrs.foreground = 0xff0000ff; break;
        case 32: term->vt.attrs.foreground = 0x00ff00ff; break;
        case 33: term->vt.attrs.foreground = 0xf0f000ff; break;
        case 34: term->vt.attrs.foreground = 0x0000ffff; break;
        case 35: term->vt.attrs.foreground = 0xf000f0ff; break;
        case 36: term->vt.attrs.foreground = 0x00f0f0ff; break;
        case 37: term->vt.attrs.foreground = 0xffffffff; break;
        case 39: term->vt.attrs.foreground = term->grid.foreground; break;

        case 40: term->vt.attrs.background = 0x000000ff; break;
        case 41: term->vt.attrs.background = 0xff0000ff; break;
        case 42: term->vt.attrs.background = 0x00ff00ff; break;
        case 43: term->vt.attrs.background = 0xf0f000ff; break;
        case 44: term->vt.attrs.background = 0x0000ffff; break;
        case 45: term->vt.attrs.background = 0xf000f0ff; break;
        case 46: term->vt.attrs.background = 0x00f0f0ff; break;
        case 47: term->vt.attrs.background = 0xffffffff; break;
        case 49: term->vt.attrs.background = term->grid.background; break;

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
