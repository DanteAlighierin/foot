#pragma once

#include "terminal.h"

const struct font *attrs_to_font(
    struct terminal *term, const struct attributes *attrs);

void grid_render(struct terminal *term);
void render_resize(struct terminal *term, int width, int height);
void render_set_title(struct terminal *term, const char *title);
void render_update_cursor_surface(struct terminal *term);
