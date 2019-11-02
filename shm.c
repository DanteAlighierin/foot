#include "shm.h"

#include <unistd.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <linux/memfd.h>

#include <pixman.h>

#define LOG_MODULE "shm"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "stride.h"
#include "tllist.h"

static tll(struct buffer) buffers;

static void
buffer_destroy(struct buffer *buf)
{
    if (buf->pix != NULL)
        pixman_image_unref(buf->pix);
    wl_buffer_destroy(buf->wl_buf);
    munmap(buf->mmapped, buf->size);
}

static void
buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    struct buffer *buffer = data;
    assert(buffer->wl_buf == wl_buffer);
    assert(buffer->busy);
    buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = &buffer_release,
};

struct buffer *
shm_get_buffer(struct wl_shm *shm, int width, int height, unsigned long cookie)
{
    tll_foreach(buffers, it) {
        if (it->item.width != width)
            continue;
        if (it->item.height != height)
            continue;
        if (it->item.cookie != cookie)
            continue;

        if (!it->item.busy) {
            LOG_DBG("cookie=%lx: re-using buffer from cache", cookie);
            it->item.busy = true;
            it->item.purge = false;
            return &it->item;
        }
    }

    /* Purge buffers marked for purging */
    tll_foreach(buffers, it) {
        if (it->item.cookie != cookie)
            continue;

        if (!it->item.purge)
            continue;

        assert(!it->item.busy);

        LOG_DBG("cookie=%lx: purging buffer %p (width=%d, height=%d)",
                cookie, &it->item, it->item.width, it->item.height);

        buffer_destroy(&it->item);
        tll_remove(buffers, it);
    }

    /* Purge old buffers associated with this cookie */
    tll_foreach(buffers, it) {
        if (it->item.cookie != cookie)
            continue;

        if (it->item.busy)
            continue;

        if (it->item.width == width && it->item.height == height)
            continue;

        LOG_DBG("cookie=%lx: marking buffer %p for purging", cookie, &it->item);
        it->item.purge = true;
    }

    /*
     * No existing buffer available. Create a new one by:
     *
     * 1. open a memory backed "file" with memfd_create()
     * 2. mmap() the memory file, to be used by the pixman image
     * 3. create a wayland shm buffer for the same memory file
     *
     * The pixman image and the wayland buffer are now sharing memory.
     */

    int pool_fd = -1;
    void *mmapped = NULL;
    size_t size = 0;

    struct wl_shm_pool *pool = NULL;
    struct wl_buffer *buf = NULL;

    pixman_image_t *pix = NULL;

    /* Backing memory for SHM */
    pool_fd = memfd_create("foot-wayland-shm-buffer-pool", MFD_CLOEXEC);
    if (pool_fd == -1) {
        LOG_ERRNO("failed to create SHM backing memory file");
        goto err;
    }

    const int stride = stride_for_format_and_width(PIXMAN_a8r8g8b8, width);

    /* Total size */
    size = stride * height;
    LOG_DBG("cookie=%lx: allocating new buffer: %zu KB", cookie, size / 1024);

    if (ftruncate(pool_fd, size) == -1) {
        LOG_ERRNO("failed to truncate SHM pool");
        goto err;
    }

    mmapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, 0);
    if (mmapped == MAP_FAILED) {
        LOG_ERR("failed to mmap SHM backing memory file");
        goto err;
    }

    pool = wl_shm_create_pool(shm, pool_fd, size);
    if (pool == NULL) {
        LOG_ERR("failed to create SHM pool");
        goto err;
    }

    buf = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    if (buf == NULL) {
        LOG_ERR("failed to create SHM buffer");
        goto err;
    }

    /* We use the entire pool for our single buffer */
    wl_shm_pool_destroy(pool); pool = NULL;
    close(pool_fd); pool_fd = -1;

    /* One pixman image for each worker thread (do we really need multiple?) */
    pix = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8, width, height, (uint32_t *)mmapped, stride);

    /* Push to list of available buffers, but marked as 'busy' */
    tll_push_back(
        buffers,
        ((struct buffer){
            .cookie = cookie,
            .width = width,
            .height = height,
            .stride = stride,
            .busy = true,
            .size = size,
            .mmapped = mmapped,
            .wl_buf = buf,
            .pix = pix}
            )
        );

    struct buffer *ret = &tll_back(buffers);
    wl_buffer_add_listener(ret->wl_buf, &buffer_listener, ret);
    return ret;

err:
    if (pix != NULL)
        pixman_image_unref(pix);
    if (buf != NULL)
        wl_buffer_destroy(buf);
    if (pool != NULL)
        wl_shm_pool_destroy(pool);
    if (pool_fd != -1)
        close(pool_fd);
    if (mmapped != NULL)
        munmap(mmapped, size);

    return NULL;
}

void
shm_fini(void)
{
    tll_foreach(buffers, it) {
        buffer_destroy(&it->item);
        tll_remove(buffers, it);
    }
}

void
shm_purge(struct wl_shm *shm, unsigned long cookie)
{
    LOG_DBG("cookie=%lx: purging all buffers", cookie);

    /* Purge old buffers associated with this cookie */
    tll_foreach(buffers, it) {
        if (it->item.cookie != cookie)
            continue;

        assert(!it->item.busy);

        buffer_destroy(&it->item);
        tll_remove(buffers, it);
    }
}
