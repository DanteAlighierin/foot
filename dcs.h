#pragma once

#include <stdbool.h>
#include "terminal.h"

void dcs_passthrough(struct terminal *term);
bool dcs_ensure_size(struct terminal *term, size_t required_size);
