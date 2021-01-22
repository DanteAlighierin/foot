#include "extract.h"
#include <stdlib.h>

#define LOG_MODULE "extract"
#define LOG_ENABLE_DBG 1
#include "log.h"

struct extraction_context {
    wchar_t *buf;
    size_t size;
    size_t idx;
    size_t empty_count;
    size_t newline_count;
    bool failed;
    const struct row *last_row;
    const struct cell *last_cell;
    enum selection_kind selection_kind;
};

struct extraction_context *
extract_begin(enum selection_kind kind)
{
    struct extraction_context *ctx = malloc(sizeof(*ctx));
    if (unlikely(ctx == NULL)) {
        LOG_ERRNO("malloc() failed");
        return NULL;
    }

    *ctx = (struct extraction_context){
        .selection_kind = kind,
    };
    return ctx;
}

static bool
ensure_size(struct extraction_context *ctx, size_t additional_chars)
{
    while (ctx->size < ctx->idx + additional_chars) {
        size_t new_size = ctx->size == 0 ? 512 : ctx->size * 2;
        wchar_t *new_buf = realloc(ctx->buf, new_size * sizeof(wchar_t));

        if (new_buf == NULL)
            return false;

        ctx->buf = new_buf;
        ctx->size = new_size;
    }

    xassert(ctx->size >= ctx->idx + additional_chars);
    return true;
}

bool
extract_finish(struct extraction_context *ctx, char **text, size_t *len)
{
    bool ret = false;

    if (text == NULL)
        return false;

    *text = NULL;
    if (len != NULL)
        *len = 0;

    if (ctx->failed)
        goto out;

    if (ctx->idx == 0) {
        /* Selection of empty cells only */
        if (!ensure_size(ctx, 1))
            goto out;
        ctx->buf[ctx->idx++] = L'\0';
    } else {
        xassert(ctx->idx > 0);
        xassert(ctx->idx <= ctx->size);
        if (ctx->buf[ctx->idx - 1] == L'\n')
            ctx->buf[ctx->idx - 1] = L'\0';
        else {
            if (!ensure_size(ctx, 1))
                goto out;
            ctx->buf[ctx->idx++] = L'\0';
        }
    }

    size_t _len = wcstombs(NULL, ctx->buf, 0);
    if (_len == (size_t)-1) {
        LOG_ERRNO("failed to convert selection to UTF-8");
        goto out;
    }

    *text = malloc(_len + 1);
    if (unlikely(text == NULL)) {
        LOG_ERRNO("malloc() failed");
        goto out;
    }

    wcstombs(*text, ctx->buf, _len + 1);

    if (len != NULL)
        *len = _len;

    ret = true;

out:
    free(ctx->buf);
    free(ctx);
    return ret;
}

bool
extract_one(const struct terminal *term, const struct row *row,
            const struct cell *cell, int col, void *context)
{
    struct extraction_context *ctx = context;

    if (cell->wc == CELL_MULT_COL_SPACER)
        return true;

    if (ctx->last_row != NULL && row != ctx->last_row) {
        /* New row - determine if we should insert a newline or not */

        if (ctx->selection_kind != SELECTION_BLOCK) {
            if (ctx->last_row->linebreak ||
                ctx->empty_count > 0 ||
                cell->wc == 0)
            {
                /* Row has a hard linebreak, or either last cell or
                 * current cell is empty */

                /* Don't emit newline just yet - only if there are
                 * non-empty cells following it */
                ctx->newline_count++;
                ctx->empty_count = 0;
            }
        } else {
            /* Always insert a linebreak */
            if (!ensure_size(ctx, 1))
                goto err;

            ctx->buf[ctx->idx++] = L'\n';
            ctx->empty_count = 0;
        }
    }

    if (cell->wc == 0) {
        ctx->empty_count++;
        ctx->last_row = row;
        ctx->last_cell = cell;
        return true;
    }

    /* Insert pending newlines, and replace empty cells with spaces */
    if (!ensure_size(ctx, ctx->newline_count + ctx->empty_count))
        goto err;

    for (size_t i = 0; i < ctx->newline_count; i++)
        ctx->buf[ctx->idx++] = L'\n';

    for (size_t i = 0; i < ctx->empty_count; i++)
        ctx->buf[ctx->idx++] = L' ';

    ctx->newline_count = 0;
    ctx->empty_count = 0;

    if (cell->wc >= CELL_COMB_CHARS_LO &&
        cell->wc < (CELL_COMB_CHARS_LO + term->composed_count))
    {
        const struct composed *composed
            = &term->composed[cell->wc - CELL_COMB_CHARS_LO];

        if (!ensure_size(ctx, 1 + composed->count))
            goto err;

        ctx->buf[ctx->idx++] = composed->base;
        for (size_t i = 0; i < composed->count; i++)
            ctx->buf[ctx->idx++] = composed->combining[i];
    }

    else {
        if (!ensure_size(ctx, 1))
            goto err;
        ctx->buf[ctx->idx++] = cell->wc;
    }

    ctx->last_row = row;
    ctx->last_cell = cell;
    return true;

err:
    ctx->failed = true;
    return false;
}
