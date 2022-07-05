#include "extract.h"
#include <string.h>

#define LOG_MODULE "extract"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"

struct extraction_context {
    char32_t *buf;
    size_t size;
    size_t idx;
    size_t tab_spaces_left;
    size_t empty_count;
    size_t newline_count;
    bool strip_trailing_empty;
    bool failed;
    const struct row *last_row;
    const struct cell *last_cell;
    enum selection_kind selection_kind;
};

struct extraction_context *
extract_begin(enum selection_kind kind, bool strip_trailing_empty)
{
    struct extraction_context *ctx = malloc(sizeof(*ctx));
    if (unlikely(ctx == NULL)) {
        LOG_ERRNO("malloc() failed");
        return NULL;
    }

    *ctx = (struct extraction_context){
        .selection_kind = kind,
        .strip_trailing_empty = strip_trailing_empty,
    };
    return ctx;
}

static bool
ensure_size(struct extraction_context *ctx, size_t additional_chars)
{
    while (ctx->size < ctx->idx + additional_chars) {
        size_t new_size = ctx->size == 0 ? 512 : ctx->size * 2;
        char32_t *new_buf = realloc(ctx->buf, new_size * sizeof(new_buf[0]));

        if (new_buf == NULL)
            return false;

        ctx->buf = new_buf;
        ctx->size = new_size;
    }

    xassert(ctx->size >= ctx->idx + additional_chars);
    return true;
}

bool
extract_finish_wide(struct extraction_context *ctx, char32_t **text, size_t *len)
{
    if (text == NULL)
        return false;

    *text = NULL;
    if (len != NULL)
        *len = 0;

    if (ctx->failed)
        goto err;

    if (!ctx->strip_trailing_empty) {
        /* Insert pending newlines, and replace empty cells with spaces */
        if (!ensure_size(ctx, ctx->newline_count + ctx->empty_count))
            goto err;

        for (size_t i = 0; i < ctx->newline_count; i++)
            ctx->buf[ctx->idx++] = U'\n';

        for (size_t i = 0; i < ctx->empty_count; i++)
            ctx->buf[ctx->idx++] = U' ';
    }

    if (ctx->idx == 0) {
        /* Selection of empty cells only */
        if (!ensure_size(ctx, 1))
            goto err;
        ctx->buf[ctx->idx++] = U'\0';
    } else {
        xassert(ctx->idx > 0);
        xassert(ctx->idx <= ctx->size);

        switch (ctx->selection_kind) {
        default:
            if (ctx->buf[ctx->idx - 1] == U'\n')
                ctx->buf[ctx->idx - 1] = U'\0';
            break;

        case SELECTION_LINE_WISE:
            if (ctx->buf[ctx->idx - 1] != U'\n') {
                if (!ensure_size(ctx, 1))
                    goto err;
                ctx->buf[ctx->idx++] = U'\n';
            }
            break;

        }

        if (ctx->buf[ctx->idx - 1] != U'\0') {
            if (!ensure_size(ctx, 1))
                goto err;
            ctx->buf[ctx->idx++] = U'\0';
        }
    }

    *text = ctx->buf;
    if (len != NULL)
        *len = ctx->idx - 1;
    free(ctx);
    return true;

err:
    free(ctx->buf);
    free(ctx);
    return false;
}

bool
extract_finish(struct extraction_context *ctx, char **text, size_t *len)
{
    if (text == NULL)
        return false;
    if (len != NULL)
        *len = 0;

    char32_t *wtext;
    if (!extract_finish_wide(ctx, &wtext, NULL))
        return false;

    bool ret = false;

    *text = ac32tombs(wtext);
    if (*text == NULL) {
        LOG_ERR("failed to convert selection to UTF-8");
        goto out;
    }

    if (len != NULL)
        *len = strlen(*text);
    ret = true;

out:
    free(wtext);
    return ret;
}

bool
extract_one(const struct terminal *term, const struct row *row,
            const struct cell *cell, int col, void *context)
{
    struct extraction_context *ctx = context;

    if (cell->wc >= CELL_SPACER)
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

                if (!ctx->strip_trailing_empty) {
                    if (!ensure_size(ctx, ctx->empty_count))
                        goto err;
                    for (size_t i = 0; i < ctx->empty_count; i++)
                        ctx->buf[ctx->idx++] = U' ';
                }
                ctx->empty_count = 0;
            }
        } else {
            /* Always insert a linebreak */
            if (!ensure_size(ctx, 1))
                goto err;

            ctx->buf[ctx->idx++] = U'\n';

            if (!ctx->strip_trailing_empty) {
                if (!ensure_size(ctx, ctx->empty_count))
                    goto err;
                for (size_t i = 0; i < ctx->empty_count; i++)
                    ctx->buf[ctx->idx++] = U' ';
            }
            ctx->empty_count = 0;
        }

        ctx->tab_spaces_left = 0;
    }

    if (cell->wc == U' ' && ctx->tab_spaces_left > 0) {
        ctx->tab_spaces_left--;
        return true;
    }

    ctx->tab_spaces_left = 0;

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
        ctx->buf[ctx->idx++] = U'\n';

    for (size_t i = 0; i < ctx->empty_count; i++)
        ctx->buf[ctx->idx++] = U' ';

    ctx->newline_count = 0;
    ctx->empty_count = 0;

    if (cell->wc >= CELL_COMB_CHARS_LO && cell->wc <= CELL_COMB_CHARS_HI)
    {
        const struct composed *composed = composed_lookup(
            term->composed, cell->wc - CELL_COMB_CHARS_LO);

        if (!ensure_size(ctx, composed->count))
            goto err;

        for (size_t i = 0; i < composed->count; i++)
            ctx->buf[ctx->idx++] = composed->chars[i];
    }

    else {
        if (!ensure_size(ctx, 1))
            goto err;
        ctx->buf[ctx->idx++] = cell->wc;

        if (cell->wc == U'\t') {
            int next_tab_stop = term->cols - 1;
            tll_foreach(term->tab_stops, it) {
                if (it->item > col) {
                    next_tab_stop = it->item;
                    break;
                }
            }

            xassert(next_tab_stop >= col);
            ctx->tab_spaces_left = next_tab_stop - col;
        }
    }

    ctx->last_row = row;
    ctx->last_cell = cell;
    return true;

err:
    ctx->failed = true;
    return false;
}
