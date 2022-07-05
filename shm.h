#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include <pixman.h>
#include <wayland-client.h>

#include <tllist.h>

struct damage;

struct buffer {
    int width;
    int height;
    int stride;

    void *data;

    struct wl_buffer *wl_buf;
    pixman_image_t **pix;
    size_t pix_instances;

    unsigned age;

    struct damage *scroll_damage;
    size_t scroll_damage_count;
    pixman_region32_t dirty;
};

void shm_fini(void);
void shm_set_max_pool_size(off_t max_pool_size);

struct buffer_chain;
struct buffer_chain *shm_chain_new(
    struct wl_shm *shm, bool scrollable, size_t pix_instances);
void shm_chain_free(struct buffer_chain *chain);

/*
 * Returns a single buffer.
 *
 * May returned a cached buffer. If so, the buffer’s age indicates how
 * many shm_get_buffer() calls have been made for the same
 * width/height while the buffer was still busy.
 *
 * A newly allocated buffer has an age of 1234.
 */
struct buffer *shm_get_buffer(struct buffer_chain *chain, int width, int height);
/*
 * Returns many buffers, described by ‘info’, all sharing the same SHM
 * buffer pool.
 *
 * Never returns cached buffers. However, the newly created buffers
 * are all inserted into the regular buffer cache, and are treated
 * just like buffers created by shm_get_buffer().
 *
 * This function is useful when allocating many small buffers, with
 * (roughly) the same life time.
 *
 * Buffers are tagged for immediate purging, and will be destroyed as
 * soon as the compositor releases them.
 */
void shm_get_many(
    struct buffer_chain *chain, size_t count,
    int widths[static count], int heights[static count],
    struct buffer *bufs[static count]);

void shm_did_not_use_buf(struct buffer *buf);

bool shm_can_scroll(const struct buffer *buf);
bool shm_scroll(struct buffer *buf, int rows,
                int top_margin, int top_keep_rows,
                int bottom_margin, int bottom_keep_rows);

void shm_addref(struct buffer *buf);
void shm_unref(struct buffer *buf);

void shm_purge(struct buffer_chain *chain);
