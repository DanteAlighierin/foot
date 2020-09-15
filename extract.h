#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "terminal.h"

struct extraction_context;

struct extraction_context *extract_begin(enum selection_kind kind);

bool extract_one(
    const struct terminal *term, const struct row *row, const struct cell *cell,
    int col, void *context);

bool extract_finish(
    struct extraction_context *context, char **text, size_t *len);
