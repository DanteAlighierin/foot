#pragma once

#include <stdbool.h>
#include "terminal.h"

bool osc_ensure_size(struct terminal *term, size_t required_size);
void osc_dispatch(struct terminal *term);
