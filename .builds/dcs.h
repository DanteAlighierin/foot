#pragma once

#include <stdbool.h>
#include "terminal.h"

void dcs_hook(struct terminal *term, uint8_t final);
void dcs_put(struct terminal *term, uint8_t c);
void dcs_unhook(struct terminal *term);
