#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include <pixman.h>
#include <wayland-client.h>

struct buffer {
    unsigned long cookie;

    int width;
    int height;
    int stride;

    bool busy;
    size_t size;           /* Buffer size */
    void *mmapped;         /* Raw data */

    struct wl_buffer *wl_buf;
    pixman_image_t *pix;

    /* Internal */
    int fd;                /* memfd */
    void *real_mmapped;    /* Address returned from mmap */
    size_t mmap_size;      /* Size of mmap (>= size) */
    off_t offset;          /* Offset into memfd where data begins */
    bool purge;            /* True if this buffer should be destroyed */
};

struct buffer *shm_get_buffer(
    struct wl_shm *shm, int width, int height, unsigned long cookie);
void shm_fini(void);

bool shm_can_scroll(void);
bool shm_scroll(struct wl_shm *shm, struct buffer *buf, int rows,
                int top_margin, int top_keep_rows,
                int bottom_margin, int bottom_keep_rows);

void shm_purge(struct wl_shm *shm, unsigned long cookie);

struct terminal;
static inline unsigned long shm_cookie_grid(const struct terminal *term) { return (unsigned long)((uintptr_t)term + 0); }
static inline unsigned long shm_cookie_search(const struct terminal *term) { return (unsigned long)((uintptr_t)term + 1); }
static inline unsigned long shm_cookie_csd(const struct terminal *term, int n) { return (unsigned long)((uintptr_t)term + 2 + (n)); }
