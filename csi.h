#pragma once

#include <stdbool.h>
#include "terminal.h"

bool csi_dispatch(struct terminal *term, uint8_t final);
