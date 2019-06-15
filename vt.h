#pragma once

#include <stdint.h>
#include <stddef.h>

#include "terminal.h"

void vt_from_slave(struct terminal *term, const uint8_t *data, size_t len);
