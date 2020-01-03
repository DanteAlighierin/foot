#pragma once

#include "terminal.h"

struct font *attrs_to_font(
    const struct terminal *term, const struct attributes *attrs);

void grid_render(struct terminal *term);
void render_resize(struct terminal *term, int width, int height, bool refresh);
void render_set_title(struct terminal *term, const char *title);
void render_refresh(struct terminal *term);

void render_search_box(struct terminal *term);

struct render_worker_context {
    int my_id;
    struct terminal *term;
};
int render_worker_thread(void *_ctx);
