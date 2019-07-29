#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <cairo.h>
#include <wayland-client.h>

struct buffer {
    int width;
    int height;

    bool busy;
    size_t size;
    void *mmapped;

    struct wl_buffer *wl_buf;

    size_t copies;
    cairo_surface_t **cairo_surface;
    cairo_t **cairo;
};

struct buffer *shm_get_buffer(struct wl_shm *shm, int width, int height, size_t copies);
void shm_fini(void);
