#include "search.h"

#include <wchar.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "search"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "render.h"

void
search_begin(struct terminal *term)
{
    LOG_DBG("search: begin");

    free(term->search.buf);
    term->search.buf = NULL;
    term->search.len = 0;
    term->search.sz = 0;
    term->is_searching = true;

    render_refresh(term);
}

void
search_cancel(struct terminal *term)
{
    LOG_DBG("search: cancel");

    free(term->search.buf);
    term->search.buf = NULL;
    term->search.len = 0;
    term->search.sz = 0;
    term->is_searching = false;

    render_refresh(term);
}

void
search_input(struct terminal *term, uint32_t key, xkb_keysym_t sym, xkb_mod_mask_t mods)
{
    LOG_DBG("search: input: sym=%d/0x%x, mods=0x%08x", sym, sym, mods);

    const xkb_mod_mask_t ctrl = 1 << term->kbd.mod_ctrl;
    //const xkb_mod_mask_t alt = 1 << term->kbd.mod_alt;
    //const xkb_mod_mask_t shift = 1 << term->kbd.mod_shift;
    //const xkb_mod_mask_t meta = 1 << term->kbd.mod_meta;

    enum xkb_compose_status compose_status = xkb_compose_state_get_status(
        term->kbd.xkb_compose_state);

    if ((mods == 0 && sym == XKB_KEY_Escape) || (mods == ctrl && sym == XKB_KEY_g))
        search_cancel(term);

    else if (mods == 0 && sym == XKB_KEY_BackSpace) {
        if (term->search.len > 0)
            term->search.buf[--term->search.len] = L'\0';
    }

    else {
        uint8_t buf[64] = {0};
        int count = 0;

        if (compose_status == XKB_COMPOSE_COMPOSED) {
            count = xkb_compose_state_get_utf8(
                term->kbd.xkb_compose_state, (char *)buf, sizeof(buf));
            xkb_compose_state_reset(term->kbd.xkb_compose_state);
        } else {
            count = xkb_state_key_get_utf8(
                term->kbd.xkb_state, key, (char *)buf, sizeof(buf));
        }

        const char *src = (const char *)buf;
        mbstate_t ps = {0};
        size_t wchars = mbsnrtowcs(NULL, &src, count, 0, &ps);

        if (wchars == -1) {
            LOG_ERRNO("failed to convert %.*s to wchars", count, buf);
            return;
        }

        while (term->search.len + wchars >= term->search.sz) {
            size_t new_sz = term->search.sz == 0 ? 64 : term->search.sz * 2;
            wchar_t *new_buf = realloc(term->search.buf, new_sz * sizeof(term->search.buf[0]));

            if (new_buf == NULL) {
                LOG_ERRNO("failed to resize search buffer");
                return;
            }

            term->search.buf = new_buf;
            term->search.sz = new_sz;
        }

        assert(term->search.len + wchars < term->search.sz);

        memset(&ps, 0, sizeof(ps));
        mbsnrtowcs(&term->search.buf[term->search.len], &src, count,
                   term->search.sz - term->search.len - 1, &ps);

        term->search.len += wchars;
        term->search.buf[term->search.len] = L'\0';
    }

    LOG_DBG("search: buffer: %S", term->search.buf);
    render_refresh(term);
}
