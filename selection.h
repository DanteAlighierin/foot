#pragma once

#include "terminal.h"

void selection_start(struct terminal *term, int col, int row);
void selection_update(struct terminal *term, int col, int row);
void selection_finalizie(struct terminal *term);
void selection_cancel(struct terminal *term);
