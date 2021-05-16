#pragma once

#include <limits.h>
#include <stdint.h>
#include <stddef.h>

#include "terminal.h"

void vt_from_slave(struct terminal *term, const uint8_t *data, size_t len);

static inline int
vt_param_get(const struct terminal *term, size_t idx, int default_value)
{
    /*
     * We zero excess bits in parsed param values. In most cases this will
     * effectively be a no-op; but it prevents negative returns for edge
     * cases involving unusually large values.
     */
    static_assert(INT_MAX >= 0x7fffffff, "POSIX requires INT_MAX >= 0x7fffffff");
    const unsigned value_mask = 0x7fffffff;

    if (term->vt.params.idx > idx) {
        unsigned value = term->vt.params.v[idx].value & value_mask;
        return value != 0 ? (int)value : default_value;
    }

    return default_value;
}
