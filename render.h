#pragma once

#include "terminal.h"
#include "fdm.h"
#include "wayland.h"

struct renderer;
struct renderer *render_init(struct fdm *fdm, struct wayland *wayl);
void render_destroy(struct renderer *renderer);

void render_resize(struct terminal *term, int width, int height);
void render_set_title(struct terminal *term, const char *title);
void render_refresh(struct terminal *term);
bool render_xcursor_set(struct terminal *term);

void render_search_box(struct terminal *term);

void render_enable_application_synchronized_updates(struct terminal *term);
void render_disable_application_synchronized_updates(struct terminal *term);

struct render_worker_context {
    int my_id;
    struct terminal *term;
};
int render_worker_thread(void *_ctx);
