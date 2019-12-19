#include "osc.h"

#include <string.h>
#include <ctype.h>

#define LOG_MODULE "osc"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "base64.h"
#include "render.h"
#include "selection.h"
#include "terminal.h"
#include "vt.h"

#define UNHANDLED() LOG_DBG("unhandled: OSC: %.*s", (int)term->vt.osc.idx, term->vt.osc.data)

static void
osc_to_clipboard(struct terminal *term, const char *target,
                 const char *base64_data)
{
    char *decoded = base64_decode(base64_data);
    LOG_DBG("decoded: %s", decoded);

    if (decoded == NULL) {
        LOG_WARN("OSC: invalid clipboard data: %s", base64_data);
        return;
    }

    bool to_clipboard = false;
    bool to_primary = false;

    if (target[0] == '\0')
        to_clipboard = true;

    for (const char *t = target; *t != '\0'; t++) {
        switch (*t) {
        case 'c':
            to_clipboard = true;
            break;

        case 's':
        case 'p':
            to_primary = true;
            break;

        default:
            LOG_WARN("unimplemented: clipboard target '%c'", *t);
            break;
        }
    }

    if (to_clipboard) {
        char *copy = strdup(decoded);
        if (!text_to_clipboard(term, copy, term->wl->input_serial))
            free(copy);
    }

    if (to_primary) {
        char *copy = strdup(decoded);
        if (!text_to_primary(term, copy, term->wl->input_serial))
            free(copy);
    }

    free(decoded);
}

struct clip_context {
    struct terminal *term;
    uint8_t buf[3];
    int idx;
};

static void
from_clipboard_cb(const char *text, size_t size, void *user)
{
    struct clip_context *ctx = user;
    struct terminal *term = ctx->term;

    assert(ctx->idx >= 0 && ctx->idx <= 2);

    const char *t = text;
    size_t left = size;

    if (ctx->idx > 0) {
        for (size_t i = ctx->idx; i < 3 && left > 0; i++, t++, left--)
            ctx->buf[ctx->idx++] = *t;

        assert(ctx->idx <= 3);
        if (ctx->idx == 3) {
            char *chunk = base64_encode(ctx->buf, 3);
            assert(chunk != NULL);
            assert(strlen(chunk) == 4);

            term_to_slave(term, chunk, 4);
            free(chunk);

            ctx->idx = 0;
        }
    }

    if (left == 0)
        return;

    assert(ctx->idx == 0);

    int remaining = left % 3;
    for (int i = remaining; i > 0; i--)
        ctx->buf[ctx->idx++] = text[size - i];
    assert(ctx->idx == remaining);

    char *chunk = base64_encode((const uint8_t *)t, left / 3 * 3);
    assert(chunk != NULL);
    assert(strlen(chunk) % 4 == 0);
    term_to_slave(term, chunk, strlen(chunk));
    free(chunk);
}

static void
from_clipboard_done(void *user)
{
    struct clip_context *ctx = user;
    struct terminal *term = ctx->term;

    if (ctx->idx > 0) {
        char res[4];
        base64_encode_final(ctx->buf, ctx->idx, res);
        term_to_slave(term, res, 4);
    }

    term_to_slave(term, "\033\\", 2);
    free(ctx);
}

static void
osc_from_clipboard(struct terminal *term, const char *source)
{
    /* Use clipboard if no source has been specified */
    char src = source[0] == '\0' ? 'c' : 0;

    for (const char *s = source; *s != '\0'; s++) {
        if (*s == 'c' || *s == 'p' || *s == 's') {
            src = *s;
            break;
        } else
            LOG_WARN("unimplemented: clipboard source '%c'", *s);
    }

    if (src == 0)
        return;

    term_to_slave(term, "\033]52;", 5);
    term_to_slave(term, &src, 1);
    term_to_slave(term, ";", 1);

    struct clip_context *ctx = malloc(sizeof(*ctx));
    *ctx = (struct clip_context) {.term = term};

    switch (src) {
    case 'c':
        text_from_clipboard(
            term, term->wl->input_serial,
            &from_clipboard_cb, &from_clipboard_done, ctx);
        break;

    case 's':
    case 'p':
        text_from_primary(term, &from_clipboard_cb, &from_clipboard_done, ctx);
        break;
    }
}

static void
osc_selection(struct terminal *term, char *string)
{
    char *p = string;
    bool clipboard_done = false;

    /* The first parameter is a string of clipbard sources/targets */
    while (*p != '\0' && !clipboard_done) {
        switch (*p) {
        case ';':
            clipboard_done = true;
            *p = '\0';
            break;
        }

        p++;
    }

    LOG_DBG("clipboard: target = %s data = %s", string, p);

    if (strlen(p) == 1 && p[0] == '?')
        osc_from_clipboard(term, string);
    else
        osc_to_clipboard(term, string, p);
}

static void
osc_flash(struct terminal *term)
{
    /* Our own private - flash */
    term_flash(term, 50);
}

static bool
parse_legacy_color(const char *string, uint32_t *color)
{
    if (string[0] != '#')
        return false;

    string++;
    const size_t len = strlen(string);

    if (len % 3 != 0)
        return false;

    const int digits = len / 3;

    int rgb[3];
    for (size_t i = 0; i < 3; i++) {
        rgb[i] = 0;
        for (size_t j = 0; j < digits; j++) {
            size_t idx = i * digits + j;
            char c = string[idx];
            rgb[i] <<= 4;

            if (!isxdigit(c))
                rgb[i] |= 0;
            else
                rgb[i] |= c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
        }

        /* Values with less than 16 bits represent the *most
         * significant bits*. I.e. the values are *not* scaled */
        rgb[i] <<= 16 - (4 * digits);
    }

    /* Re-scale to 8-bit */
    uint8_t r = 255 * (rgb[0] / 65535.);
    uint8_t g = 255 * (rgb[1] / 65535.);
    uint8_t b = 255 * (rgb[2] / 65535.);

    LOG_DBG("legacy: %02x%02x%02x", r, g, b);
    *color = r << 16 | g << 8 | b;
    return true;
}

static bool
parse_rgb(const char *string, uint32_t *color)
{
    size_t len = strlen(string);

    /* Verify we have the minimum required length (for "rgb:x/x/x") */
    if (len < 3 /* 'rgb' */ + 1 /* ':' */ + 2 /* '/' */ + 3 * 1 /* 3 * 'x' */)
        return false;

    /* Verify prefix is "rgb:" */
    if (string[0] != 'r' || string[1] != 'g' || string[2] != 'b' || string[3] != ':')
        return false;

    string += 4;
    len -= 4;

    int rgb[3];
    int digits[3];

    for (size_t i = 0; i < 3; i++) {
        for (rgb[i] = 0, digits[i] = 0;
             len > 0 && *string != '/';
             len--, string++, digits[i]++)
        {
            char c = *string;
            rgb[i] <<= 4;

            if (!isxdigit(c))
                rgb[i] |= 0;
            else
                rgb[i] |= c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
        }

        if (i >= 2)
            break;

        if (len == 0 || *string != '/')
            return false;
        string++; len--;
    }

    /* Re-scale to 8-bit */
    uint8_t r = 255 * (rgb[0] / (double)((1 << (4 * digits[0])) - 1));
    uint8_t g = 255 * (rgb[1] / (double)((1 << (4 * digits[1])) - 1));
    uint8_t b = 255 * (rgb[2] / (double)((1 << (4 * digits[2])) - 1));

    LOG_DBG("rgb: %02x%02x%02x", r, g, b);
    *color = r << 16 | g << 8 | b;
    return true;
}

#if 0
static void
osc_notify(struct terminal *term, char *string)
{
    char *ctx = NULL;
    const char *cmd = strtok_r(string, ";", &ctx);
    const char *title = strtok_r(NULL, ";", &ctx);
    const char *msg = strtok_r(NULL, ";", &ctx);

    LOG_DBG("cmd: \"%s\", title: \"%s\", msg: \"%s\"",
            cmd, title, msg);

    if (cmd == NULL || strcmp(cmd, "notify") != 0 || title == NULL || msg == NULL)
        return;
}
#endif

void
osc_dispatch(struct terminal *term)
{
    unsigned param = 0;
    int data_ofs = 0;

    for (size_t i = 0; i < term->vt.osc.idx; i++, data_ofs++) {
        char c = term->vt.osc.data[i];

        if (c == ';') {
            data_ofs++;
            break;
        }

        if (!isdigit(c)) {
            UNHANDLED();
            return;
        }

        param *= 10;
        param += c - '0';
    }

    LOG_DBG("OCS: %.*s (param = %d)",
            (int)term->vt.osc.idx, term->vt.osc.data, param);

    char *string = (char *)&term->vt.osc.data[data_ofs];

    switch (param) {
    case 0: term_set_window_title(term, string); break;  /* icon + title */
    case 1: break;                                       /* icon */
    case 2: term_set_window_title(term, string); break;  /* title */

    case 4: {
        /* Set color<idx> */

        /* First param - the color index */
        unsigned idx = 0;
        for (; *string != '\0' && *string != ';'; string++) {
            char c = *string;
            idx *= 10;
            idx += c - '0';
        }

        if (idx >= 256)
            break;

        /* Next follows the color specification. For now, we only support rgb:x/y/z */

        if (*string == '\0') {
            /* No color specification */
            break;
        }

        assert(*string == ';');
        string++;

        /* Client queried for current value */
        if (strlen(string) == 1 && string[0] == '?') {
            uint32_t color = term->colors.table[idx];
            uint8_t r = (color >> 16) & 0xff;
            uint8_t g = (color >>  8) & 0xff;
            uint8_t b = (color >>  0) & 0xff;

            char reply[32];
            snprintf(reply, sizeof(reply), "\033]4;%u;rgb:%02x/%02x/%02x\033\\",
                     idx, r, g, b);
            term_to_slave(term, reply, strlen(reply));
            break;
        }

        uint32_t color;
        if (string[0] == '#' ? !parse_legacy_color(string, &color) : !parse_rgb(string, &color))
            break;

        LOG_DBG("change color definition for #%u to %06x", idx, color);
        term->colors.table[idx] = color;
        render_refresh(term);
        break;
    }

    case 10:
    case 11: {
        /* Set default foreground/background color */

        /* Client queried for current value */
        if (strlen(string) == 1 && string[0] == '?') {
            uint32_t color = param == 10 ? term->colors.fg : term->colors.bg;
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

            term_to_slave(term, reply, strlen(reply));
            break;
        }

        uint32_t color;
        if (string[0] == '#' ? !parse_legacy_color(string, &color) : !parse_rgb(string, &color))
            break;

        LOG_DBG("change color definition for %s to %06x",
                param == 10 ? "foreground" : "background", color);

        switch (param) {
        case 10: term->colors.fg = color; break;
        case 11: term->colors.bg = color; break;
        }

        render_refresh(term);
        break;
    }

    case 12: /* Set cursor color */
        break;

    case 30:  /* Set tab title */
        break;

    case 52:  /* Copy to/from clipboard/primary */
        osc_selection(term, string);
        break;

    case 104: {
        /* Reset Color Number 'c' (whole table if no parameter) */

        if (strlen(string) == 0) {
            LOG_DBG("resetting all colors");
            for (size_t i = 0; i < 256; i++)
                term->colors.table[i] = term->colors.default_table[i];
        } else {
            unsigned idx = 0;

            for (; *string != '\0'; string++) {
                char c = *string;
                if (c == ';') {
                    LOG_DBG("resetting color #%u", idx);
                    term->colors.table[idx] = term->colors.default_table[idx];
                    idx = 0;
                    continue;
                }

                idx *= 10;
                idx += c - '0';
            }

            LOG_DBG("resetting color #%u", idx);
            term->colors.table[idx] = term->colors.default_table[idx];
        }

        render_refresh(term);
        break;
    }

    case 105: /* Reset Special Color Number 'c' */
        break;

    case 110: /* Reset default text foreground color */
        LOG_DBG("resetting foreground");
        term->colors.fg = term->colors.default_fg;
        render_refresh(term);
        break;

    case 111: /* Reset default text background color */
        LOG_DBG("resetting background");
        term->colors.bg = term->colors.default_bg;
        render_refresh(term);
        break;

    case 555:
        osc_flash(term);
        break;

#if 0
    case 777:
        osc_notify(term, string);
        break;
#endif

    default:
        UNHANDLED();
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
