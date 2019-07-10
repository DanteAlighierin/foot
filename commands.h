#pragma once

#include "terminal.h"

void cmd_scrollback_up(struct terminal *term, int rows);
void cmd_scrollback_down(struct terminal *term, int rows);
