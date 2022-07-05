#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <uchar.h>

#include "terminal.h"

struct extraction_context;

struct extraction_context *extract_begin(
    enum selection_kind kind, bool strip_trailing_empty);

bool extract_one(
    const struct terminal *term, const struct row *row, const struct cell *cell,
    int col, void *context);

bool extract_finish(
    struct extraction_context *context, char **text, size_t *len);
bool extract_finish_wide(
    struct extraction_context *context, char32_t **text, size_t *len);
