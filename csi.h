#pragma once

#include <stdbool.h>
#include "terminal.h"

void csi_dispatch(struct terminal *term, uint8_t final);
