#pragma once

#include <stdint.h>
#include <stddef.h>

#include "terminal.h"

void vt_from_slave(struct terminal *term, const uint8_t *data, size_t len);

static inline int
vt_param_get(const struct terminal *term, size_t idx, int default_value)
{
    if (term->vt.params.idx > idx) {
        int value = term->vt.params.v[idx].value;
        return value != 0 ? value : default_value;
    }

    return default_value;
}
