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

static void
osc_to_clipboard(struct terminal *term, const char *target,
                 const char *base64_data)
{
    char *decoded = base64_decode(base64_data);
    LOG_DBG("decoded: %s", decoded);

    if (decoded == NULL) {
        LOG_WARN("OSC: invalid clipboard data: %s", base64_data);
        /* TODO: clear selection */
        abort();
        return;
    }

    for (const char *t = target; *t != '\0'; t++) {
        switch (*t) {
        case 'c': {
            char *copy = strdup(decoded);
            if (!text_to_clipboard(term, copy, term->input_serial))
                free(copy);
            break;
        }

        default:
            LOG_WARN("unimplemented: clipboard target '%c'", *t);
            break;
        }
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

            vt_to_slave(term, chunk, 4);
            free(chunk);

            ctx->idx = 0;
        }
    }

    if (left == 0)
        return;

    int remaining = left % 3;
    for (int i = remaining; i > 0; i--)
        ctx->buf[0] = text[size - i];
    ctx->idx = remaining;

    char *chunk = base64_encode((const uint8_t *)t, left / 3 * 3);
    assert(chunk != NULL);
    assert(strlen(chunk) % 4 == 0);
    vt_to_slave(term, chunk, strlen(chunk));
    free(chunk);
}

static void
osc_from_clipboard(struct terminal *term, const char *source)
{
    char src = 0;

    for (const char *s = source; *s != '\0'; s++) {
        if (*s == 'c') {
            src = 'c';
            break;
        } else if (*s == 'p') {
            src = 'p';
            break;
        }
    }

    if (src == 0)
        return;

    vt_to_slave(term, "\033]52;", 5);
    vt_to_slave(term, &src, 1);
    vt_to_slave(term, ";", 1);

    struct clip_context ctx = {
        .term = term,
    };

    switch (src) {
    case 'c':
        text_from_clipboard(term, term->input_serial, &from_clipboard_cb, &ctx);
        break;

    case 'p':
        LOG_ERR("unimplemented: osc from primary");
        abort();
        // text_from_primary(term, term->input_serial, &from_clipboard_cb, &ctx);
        break;
    }

    if (ctx.idx > 0) {
        char res[4];
        base64_encode_final(ctx.buf, ctx.idx, res);
        vt_to_slave(term, res, 4);
    }

    vt_to_slave(term, "\033\\", 2);

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

    char *string = (char *)&term->vt.osc.data[data_ofs];

    if (strlen(string) == 1 && string[0] == '?') {
        osc_query(term, param);
        return;
    }

    switch (param) {
    case 0: render_set_title(term, string); break;  /* icon + title */
    case 1: break;                                             /* icon */
    case 2: render_set_title(term, string); break;  /* title */
    case 52: osc_selection(term, string); break;

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
