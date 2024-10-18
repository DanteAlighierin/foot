#include "dcs.h"
#include <string.h>

#define LOG_MODULE "dcs"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "foot-terminfo.h"
#include "sixel.h"
#include "util.h"
#include "vt.h"
#include "xmalloc.h"
#include "xsnprintf.h"

static bool
ensure_size(struct terminal *term, size_t required_size)
{
    if (required_size <= term->vt.dcs.size)
        return true;

    uint8_t *new_data = realloc(term->vt.dcs.data, required_size);
    if (new_data == NULL) {
        LOG_ERRNO("failed to increase size of DCS buffer");
        return false;
    }

    term->vt.dcs.data = new_data;
    term->vt.dcs.size = required_size;
    return true;
}

/* Decode hex-encoded string *inline*. NULL terminates */
static char *
hex_decode(const char *s, size_t len)
{
    if (len % 2)
        return NULL;

    char *hex = xmalloc(len / 2 + 1);
    char *o = hex;

    /* TODO: error checking */
    for (size_t i = 0; i < len; i += 2) {
        uint8_t nib1 = hex2nibble(*s); s++;
        uint8_t nib2 = hex2nibble(*s); s++;

        if (nib1 == HEX_DIGIT_INVALID || nib2 == HEX_DIGIT_INVALID)
            goto err;

        *o = nib1 << 4 | nib2; o++;
    }

    *o = '\0';
    return hex;

err:
    free(hex);
    return NULL;
}

UNITTEST
{
    /* Verify table is sorted */
    const char *p = terminfo_capabilities;
    size_t left = sizeof(terminfo_capabilities);

    const char *last_cap = NULL;

    while (left > 0) {
        const char *cap = p;
        const char *val = cap + strlen(cap) + 1;

        size_t size = strlen(cap) + 1 + strlen(val) + 1;;
        xassert(size <= left);
        p += size;
        left -= size;

        if (last_cap != NULL)
            xassert(strcmp(last_cap, cap) < 0);

        last_cap = cap;
    }
}

static bool
lookup_capability(const char *name, const char **value)
{
    const char *p = terminfo_capabilities;
    size_t left = sizeof(terminfo_capabilities);

    while (left > 0) {
        const char *cap = p;
        const char *val = cap + strlen(cap) + 1;

        size_t size = strlen(cap) + 1 + strlen(val) + 1;;
        xassert(size <= left);
        p += size;
        left -= size;

        int r = strcmp(cap, name);
        if (r == 0) {
            *value = val;
            return true;
        } else if (r > 0)
            break;
    }

    *value = NULL;
    return false;
}

static void
xtgettcap_reply(struct terminal *term, const char *hex_cap_name, size_t len)
{
    char *name = hex_decode(hex_cap_name, len);
    if (name == NULL) {
        LOG_WARN("XTGETTCAP: invalid hex encoding, ignoring capability");
        return;
    }

    const char *value;
    bool valid_capability = lookup_capability(name, &value);
    xassert(!valid_capability || value != NULL);

    LOG_DBG("XTGETTCAP: cap=%s (%.*s), value=%s",
            name, (int)len, hex_cap_name,
            valid_capability ? value : "<invalid>");

    if (!valid_capability)
        goto err;

    if (value[0] == '\0') {
        /* Boolean */
        term_to_slave(term, "\033P1+r", 5);
        term_to_slave(term, hex_cap_name, len);
        term_to_slave(term, "\033\\", 2);
        goto out;
    }

    /*
     * Reply format:
     *    \EP 1 + r cap=value \E\\
     * Where 'cap' and 'value are hex encoded ascii strings
     */
    char *reply = xmalloc(
        5 +                           /* DCS 1 + r (\EP1+r) */
        len +                         /* capability name, hex encoded */
        1 +                           /* '=' */
        strlen(value) * 2 +           /* capability value, hex encoded */
        2 +                           /* ST (\E\\) */
        1);

    int idx = sprintf(reply, "\033P1+r%.*s=", (int)len, hex_cap_name);

    for (const char *c = value; *c != '\0'; c++) {
        uint8_t nib1 = (uint8_t)*c >> 4;
        uint8_t nib2 = (uint8_t)*c & 0xf;

        reply[idx] = nib1 >= 0xa ? 'A' + nib1 - 0xa : '0' + nib1; idx++;
        reply[idx] = nib2 >= 0xa ? 'A' + nib2 - 0xa : '0' + nib2; idx++;
    }

    reply[idx] = '\033'; idx++;
    reply[idx] = '\\'; idx++;
    term_to_slave(term, reply, idx);

    free(reply);
    goto out;

err:
    term_to_slave(term, "\033P0+r", 5);
    term_to_slave(term, hex_cap_name, len);
    term_to_slave(term, "\033\\", 2);

out:
    free(name);
}

static void
xtgettcap_put(struct terminal *term, uint8_t c)
{
    struct vt *vt = &term->vt;

    /* Grow buffer expontentially */
    if (vt->dcs.idx >= vt->dcs.size) {
        size_t new_size = vt->dcs.size * 2;
        if (new_size == 0)
            new_size = 128;

        if (!ensure_size(term, new_size))
            return;
    }

    vt->dcs.data[vt->dcs.idx++] = c;
}

static void
xtgettcap_unhook(struct terminal *term)
{
    size_t left = term->vt.dcs.idx;

    const char *const end = (const char *)&term->vt.dcs.data[left];
    const char *p = (const char *)term->vt.dcs.data;

    if (p == NULL) {
        /* Request is empty; send an error reply, without any capabilities */
        term_to_slave(term, "\033P0+r\033\\", 7);
        return;
    }

    while (true) {
        const char *sep = memchr(p, ';', left);
        size_t cap_len;

        if (sep == NULL) {
            /* Last capability */
            cap_len = end - p;
        } else {
            cap_len = sep - p;
        }

        xtgettcap_reply(term, p, cap_len);

        left -= cap_len + 1;
        p += cap_len + 1;

        if (sep == NULL)
            break;
    }
}

static void NOINLINE
append_sgr_attr_n(char **reply, size_t *len, const char *attr, size_t n)
{
    size_t new_len = *len + n + 1;
    *reply = xrealloc(*reply, new_len);
    memcpy(&(*reply)[*len], attr, n);
    (*reply)[new_len - 1] = ';';
    *len = new_len;
}

static void
decrqss_put(struct terminal *term, uint8_t c)
{
    /* Largest request we support is two bytes */
    if (!ensure_size(term, 2))
        return;

    struct vt *vt = &term->vt;
    if (vt->dcs.idx >= 2)
        return;
    vt->dcs.data[vt->dcs.idx++] = c;
}

static void
decrqss_unhook(struct terminal *term)
{
    const uint8_t *query = term->vt.dcs.data;
    const size_t n = term->vt.dcs.idx;

    /*
     * A note on the Ps parameter in the reply: many DEC manual
     * instances (e.g. https://vt100.net/docs/vt510-rm/DECRPSS) claim
     * that 0 means "request is valid", and 1 means "request is
     * invalid".
     *
     * However, this appears to be a typo; actual hardware inverts the
     * response (as does XTerm and mlterm):
     * https://github.com/hackerb9/vt340test/issues/13
     */

    if (n == 1 && query[0] == 'r') {
        /* DECSTBM - Set Top and Bottom Margins */
        char reply[64];
        size_t len = xsnprintf(reply, sizeof(reply), "\033P1$r%d;%dr\033\\",
                            term->scroll_region.start + 1,
                            term->scroll_region.end);
        term_to_slave(term, reply, len);
    }

    else if (n == 1 && query[0] == 'm') {
        /* SGR - Set Graphic Rendition */
        char *reply = NULL;
        size_t len = 0;

        #define append_sgr_attr(num_as_str) \
            append_sgr_attr_n(&reply, &len, num_as_str, sizeof(num_as_str) - 1)

        /* Always present, both in the example from the VT510 manual
         * (https://vt100.net/docs/vt510-rm/DECRPSS), and in XTerm and
         * mlterm */
        append_sgr_attr("0");

        struct attributes *a = &term->vt.attrs;
        if (a->bold)
            append_sgr_attr("1");
        if (a->dim)
            append_sgr_attr("2");
        if (a->italic)
            append_sgr_attr("3");
        if (a->underline) {
            if (term->vt.underline.style > UNDERLINE_SINGLE) {
                char value[4];
                size_t val_len =
                    xsnprintf(value, sizeof(value), "4:%d", term->vt.underline.style);
                append_sgr_attr_n(&reply, &len, value, val_len);
            } else
                append_sgr_attr("4");
        }
        if (a->blink)
            append_sgr_attr("5");
        if (a->reverse)
            append_sgr_attr("7");
        if (a->conceal)
            append_sgr_attr("8");
        if (a->strikethrough)
            append_sgr_attr("9");

        switch (a->fg_src) {
        case COLOR_DEFAULT:
            break;

        case COLOR_BASE16: {
            char value[4];
            size_t val_len = xsnprintf(
                value, sizeof(value), "%u",
                a->fg >= 8 ? a->fg - 8 + 90 : a->fg + 30);
            append_sgr_attr_n(&reply, &len, value, val_len);
            break;
        }

        case COLOR_BASE256: {
            char value[16];
            size_t val_len = xsnprintf(value, sizeof(value), "38:5:%u", a->fg);
            append_sgr_attr_n(&reply, &len, value, val_len);
            break;
        }

        case COLOR_RGB: {
            uint8_t r = a->fg >> 16;
            uint8_t g = a->fg >> 8;
            uint8_t b = a->fg >> 0;

            char value[32];
            size_t val_len = xsnprintf(
                value, sizeof(value), "38:2::%hhu:%hhu:%hhu", r, g, b);
            append_sgr_attr_n(&reply, &len, value, val_len);
            break;
        }
        }

        switch (a->bg_src) {
        case COLOR_DEFAULT:
            break;

        case COLOR_BASE16: {
            char value[4];
            size_t val_len = xsnprintf(
                value, sizeof(value), "%u",
                a->bg >= 8 ? a->bg - 8 + 100 : a->bg + 40);
            append_sgr_attr_n(&reply, &len, value, val_len);
            break;
        }

        case COLOR_BASE256: {
            char value[16];
            size_t val_len = xsnprintf(value, sizeof(value), "48:5:%u", a->bg);
            append_sgr_attr_n(&reply, &len, value, val_len);
            break;
        }

        case COLOR_RGB: {
            uint8_t r = a->bg >> 16;
            uint8_t g = a->bg >> 8;
            uint8_t b = a->bg >> 0;

            char value[32];
            size_t val_len = xsnprintf(
                value, sizeof(value), "48:2::%hhu:%hhu:%hhu", r, g, b);
            append_sgr_attr_n(&reply, &len, value, val_len);
            break;
        }
        }

        switch (term->vt.underline.color_src) {
        case COLOR_DEFAULT:
        case COLOR_BASE16:
            break;

        case COLOR_BASE256: {
            char value[16];
            size_t val_len = xsnprintf(
                value, sizeof(value), "58:5:%u", term->vt.underline.color);
            append_sgr_attr_n(&reply, &len, value, val_len);
            break;
        }

        case COLOR_RGB: {
            uint8_t r = term->vt.underline.color >> 16;
            uint8_t g = term->vt.underline.color >> 8;
            uint8_t b = term->vt.underline.color >> 0;

            char value[32];
            size_t val_len = xsnprintf(
                value, sizeof(value), "58:2::%hhu:%hhu:%hhu", r, g, b);
            append_sgr_attr_n(&reply, &len, value, val_len);
            break;
        }
        }

        #undef append_sgr_attr_n

        reply[len - 1] = 'm';

        term_to_slave(term, "\033P1$r", 5);
        term_to_slave(term, reply, len);
        term_to_slave(term, "\033\\", 2);
        free(reply);
    }

    else if (n == 2 && memcmp(query, " q", 2) == 0) {
        /* DECSCUSR - Set Cursor Style */
        int mode;

        switch (term->cursor_style) {
        case CURSOR_BLOCK:     mode = 2; break;
        case CURSOR_UNDERLINE: mode = 4; break;
        case CURSOR_BEAM:      mode = 6; break;
        default: BUG("invalid cursor style"); break;
        }

        if (term->cursor_blink.deccsusr)
            mode--;

        char reply[16];
        size_t len = xsnprintf(reply, sizeof(reply), "\033P1$r%d q\033\\", mode);
        term_to_slave(term, reply, len);
    }

    else {
        static const char err[] = "\033P0$r\033\\";
        term_to_slave(term, err, sizeof(err) - 1);
    }
}

void
dcs_hook(struct terminal *term, uint8_t final)
{
    LOG_DBG("hook: %c (intermediate(s): %.2s, param=%d)", final,
            (const char *)&term->vt.private, vt_param_get(term, 0, 0));

    xassert(term->vt.dcs.data == NULL);
    xassert(term->vt.dcs.size == 0);
    xassert(term->vt.dcs.put_handler == NULL);
    xassert(term->vt.dcs.unhook_handler == NULL);

    switch (term->vt.private) {
    case 0:
        switch (final) {
        case 'q': {
            if (!term->conf->tweak.sixel) {
                break;
            }
            int p1 = vt_param_get(term, 0, 0);
            int p2 = vt_param_get(term, 1,0);
            int p3 = vt_param_get(term, 2, 0);

            term->vt.dcs.put_handler = sixel_init(term, p1, p2, p3);
            term->vt.dcs.unhook_handler = &sixel_unhook;
            break;
        }
        }
        break;

    case '$':
        switch (final) {
        case 'q':
            term->vt.dcs.put_handler = &decrqss_put;
            term->vt.dcs.unhook_handler = &decrqss_unhook;
            break;
        }
        break;

    case '=':
        switch (final) {
        case 's':
            /* BSU/ESU: https://gitlab.com/gnachman/iterm2/-/wikis/synchronized-updates-spec */
            switch (vt_param_get(term, 0, 0)) {
            case 1:
                term->vt.dcs.unhook_handler = &term_enable_app_sync_updates;
                return;
            case 2:
                term->vt.dcs.unhook_handler = &term_disable_app_sync_updates;
                return;
            }
            break;
        }
        break;

    case '+':
        switch (final) {
        case 'q':  /* XTGETTCAP */
            term->vt.dcs.put_handler = &xtgettcap_put;
            term->vt.dcs.unhook_handler = &xtgettcap_unhook;
            break;
        }
        break;
    }
}

void
dcs_put(struct terminal *term, uint8_t c)
{
    /* LOG_DBG("PUT: %c", c); */

    if (term->vt.dcs.put_handler != NULL)
        term->vt.dcs.put_handler(term, c);
}

void
dcs_unhook(struct terminal *term)
{
    if (term->vt.dcs.unhook_handler != NULL)
        term->vt.dcs.unhook_handler(term);

    term->vt.dcs.unhook_handler = NULL;
    term->vt.dcs.put_handler = NULL;

    free(term->vt.dcs.data);
    term->vt.dcs.data = NULL;
    term->vt.dcs.size = 0;
    term->vt.dcs.idx = 0;
}
