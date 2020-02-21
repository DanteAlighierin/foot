#pragma once

#include "terminal.h"

void sixel_init(struct terminal *term);
void sixel_put(struct terminal *term, uint8_t c);
void sixel_unhook(struct terminal *term);
