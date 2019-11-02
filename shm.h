#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <pixman.h>
#include <wayland-client.h>

struct buffer {
    unsigned long cookie;

    int width;
    int height;
    int stride;

    bool purge;

    bool busy;
    size_t size;
    void *mmapped;

    struct wl_buffer *wl_buf;
    pixman_image_t *pix;
};

struct buffer *shm_get_buffer(
    struct wl_shm *shm, int width, int height, unsigned long cookie);
void shm_fini(void);

void shm_purge(struct wl_shm *shm, unsigned long cookie);
