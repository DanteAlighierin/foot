#pragma once

#include "terminal.h"

struct font *attrs_to_font(
    struct terminal *term, const struct attributes *attrs);

void grid_render(struct terminal *term);
void render_resize(struct terminal *term, int width, int height, int scale);
void render_set_title(struct terminal *term, const char *title);
void render_update_cursor_surface(struct terminal *term);
void render_refresh(struct terminal *term);

struct render_worker_context {
    int my_id;
    struct terminal *term;
};
int render_worker_thread(void *_ctx);
