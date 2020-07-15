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
    bool failed;
    const struct row *last_row;
    const struct cell *last_cell;
    enum selection_kind selection_kind;
};

struct extraction_context *
extract_begin(enum selection_kind kind)
{
    struct extraction_context *ctx = malloc(sizeof(*ctx));
    *ctx = (struct extraction_context){
        .selection_kind = kind,
    };
    return ctx;
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
        ctx->buf[ctx->idx] = L'\0';
    } else {
        assert(ctx->idx > 0);
        assert(ctx->idx < ctx->size);
        if (ctx->buf[ctx->idx - 1] == L'\n')
            ctx->buf[ctx->idx - 1] = L'\0';
        else
            ctx->buf[ctx->idx] = L'\0';
    }

    size_t _len = wcstombs(NULL, ctx->buf, 0);
    if (_len == (size_t)-1) {
        LOG_ERRNO("failed to convert selection to UTF-8");
        goto out;
    }

    *text = malloc(_len + 1);
    wcstombs(*text, ctx->buf, _len + 1);

    if (len != NULL)
        *len = _len;

    ret = true;

out:
    free(ctx->buf);
    free(ctx);
    return ret;
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

    assert(ctx->size >= ctx->idx + additional_chars);
    return true;
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

        if (ctx->selection_kind == SELECTION_NONE ||
            ctx->selection_kind == SELECTION_NORMAL)
        {
            if (ctx->last_row->linebreak ||
                ctx->empty_count > 0 ||
                cell->wc == 0)
            {
                /* Row has a hard linebreak, or either last cell or
                 * current cell is empty */
                if (!ensure_size(ctx, 1))
                    goto err;

                ctx->buf[ctx->idx++] = L'\n';
                ctx->empty_count = 0;
            }
        }

        else if (ctx->selection_kind == SELECTION_BLOCK) {
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

    /* Replace empty cells with spaces when followed by non-empty cell */
    if (!ensure_size(ctx, ctx->empty_count))
        goto err;

    for (size_t i = 0; i < ctx->empty_count; i++)
        ctx->buf[ctx->idx++] = L' ';
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
