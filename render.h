#pragma once
#include <stdbool.h>

#include "terminal.h"
#include "fdm.h"
#include "wayland.h"
#include "misc.h"

struct renderer;
struct renderer *render_init(struct fdm *fdm, struct wayland *wayl);
void render_destroy(struct renderer *renderer);

bool render_resize(struct terminal *term, int width, int height);
bool render_resize_force(struct terminal *term, int width, int height);

void render_refresh(struct terminal *term);
void render_refresh_csd(struct terminal *term);
void render_refresh_search(struct terminal *term);
void render_refresh_title(struct terminal *term);
void render_refresh_urls(struct terminal *term);
bool render_xcursor_set(
    struct seat *seat, struct terminal *term, const char *xcursor);
bool render_xcursor_is_valid(const struct seat *seat, const char *cursor);

struct render_worker_context {
    int my_id;
    struct terminal *term;
};
int render_worker_thread(void *_ctx);

struct csd_data {
    int x;
    int y;
    int width;
    int height;
};

struct csd_data get_csd_data(const struct terminal *term, enum csd_surface surf_idx);
