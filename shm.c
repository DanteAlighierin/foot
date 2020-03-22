#include "shm.h"

#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/mman.h>
#include <linux/memfd.h>
#include <fcntl.h>

#include <pixman.h>

#include <fcft/stride.h>
#include <tllist.h>

#define LOG_MODULE "shm"
#define LOG_ENABLE_DBG 0
#include "log.h"

#define TIME_SCROLL 0

static tll(struct buffer) buffers;

static bool can_punch_hole = false;
static bool can_punch_hole_initialized = false;

static void
buffer_destroy(struct buffer *buf)
{
    if (buf->pix != NULL)
        pixman_image_unref(buf->pix);
    if (buf->wl_buf != NULL)
        wl_buffer_destroy(buf->wl_buf);
    if (buf->real_mmapped != MAP_FAILED)
        munmap(buf->real_mmapped, buf->mmap_size);
    if (buf->fd >= 0)
        close(buf->fd);
}

static void
buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    struct buffer *buffer = data;
    LOG_DBG("release: cookie=%lx (buf=%p)", buffer->cookie, buffer);
    assert(buffer->wl_buf == wl_buffer);
    assert(buffer->busy);
    buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = &buffer_release,
};

static bool
instantiate_offset(struct wl_shm *shm, struct buffer *buf, size_t new_offset)
{
    assert(buf->fd >= 0);
    assert(buf->mmapped == NULL);
    assert(buf->real_mmapped == NULL);
    assert(buf->wl_buf == NULL);
    assert(buf->pix == NULL);

    assert(new_offset >= buf->offset);

    static size_t page_size = 0;
    if (page_size == 0) {
        page_size = sysconf(_SC_PAGE_SIZE);
        if (page_size < 0) {
            LOG_ERRNO("failed to get page size");
            page_size = 4096;
        }
    }
    assert(page_size > 0);

    void *real_mmapped = MAP_FAILED;
    void *mmapped = MAP_FAILED;
    struct wl_shm_pool *pool = NULL;
    struct wl_buffer *wl_buf = NULL;
    pixman_image_t *pix = NULL;

    /* mmap offset must be page aligned */
    size_t aligned_offset = new_offset & ~(page_size - 1);
    size_t page_offset = new_offset & (page_size - 1);
    size_t mmap_size = buf->size + page_offset;

    assert(aligned_offset <= new_offset);
    assert(mmap_size >= buf->size);

    LOG_DBG("size=%zx, offset=%zx, size-aligned=%zx, offset-aligned=%zx",
            buf->size, buf->offset, mmap_size, aligned_offset);

    real_mmapped = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_UNINITIALIZED, buf->fd, aligned_offset);
    if (real_mmapped == MAP_FAILED) {
        LOG_ERRNO("failed to mmap SHM backing memory file");
        goto err;
    }
    mmapped = real_mmapped + page_offset;

    pool = wl_shm_create_pool(shm, buf->fd, new_offset + buf->size);
    if (pool == NULL) {
        LOG_ERR("failed to create SHM pool");
        goto err;
    }

    wl_buf = wl_shm_pool_create_buffer(
        pool, new_offset, buf->width, buf->height, buf->stride, WL_SHM_FORMAT_ARGB8888);
    if (wl_buf == NULL) {
        LOG_ERR("failed to create SHM buffer");
        goto err;
    }

    /* We use the entire pool for our single buffer */
    wl_shm_pool_destroy(pool); pool = NULL;

    /* One pixman image for each worker thread (do we really need multiple?) */
    pix = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8, buf->width, buf->height, (uint32_t *)mmapped, buf->stride);
    if (pix == NULL) {
        LOG_ERR("failed to create pixman image");
        goto err;
    }

    buf->offset = new_offset;
    buf->real_mmapped = real_mmapped;
    buf->mmapped = mmapped;
    buf->mmap_size = mmap_size;
    buf->wl_buf = wl_buf;
    buf->pix = pix;

    wl_buffer_add_listener(wl_buf, &buffer_listener, buf);
    return true;

err:
    if (pix != NULL)
        pixman_image_unref(pix);
    if (wl_buf != NULL)
        wl_buffer_destroy(wl_buf);
    if (pool != NULL)
        wl_shm_pool_destroy(pool);
    if (real_mmapped != MAP_FAILED)
        munmap(real_mmapped, mmap_size);

    abort();
    return false;
}

struct buffer *
shm_get_buffer(struct wl_shm *shm, int width, int height, unsigned long cookie)
{
    /* Purge buffers marked for purging */
    tll_foreach(buffers, it) {
        if (it->item.cookie != cookie)
            continue;

        if (!it->item.purge)
            continue;

        assert(!it->item.busy);

        LOG_DBG("cookie=%lx: purging buffer %p (width=%d, height=%d): %zu KB",
                cookie, &it->item, it->item.width, it->item.height,
                it->item.size / 1024);

        buffer_destroy(&it->item);
        tll_remove(buffers, it);
    }

    tll_foreach(buffers, it) {
        if (it->item.width != width)
            continue;
        if (it->item.height != height)
            continue;
        if (it->item.cookie != cookie)
            continue;

        if (!it->item.busy) {
            LOG_DBG("cookie=%lx: re-using buffer from cache (buf=%p)",
                    cookie, &it->item);
            it->item.busy = true;
            it->item.purge = false;
            return &it->item;
        }
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
    const int stride = stride_for_format_and_width(PIXMAN_a8r8g8b8, width);
    const size_t size = stride * height;

    LOG_DBG("cookie=%lx: allocating new buffer: %zu KB", cookie, size / 1024);

    /* Backing memory for SHM */
    pool_fd = memfd_create("foot-wayland-shm-buffer-pool", MFD_CLOEXEC);
    if (pool_fd == -1) {
        LOG_ERRNO("failed to create SHM backing memory file");
        goto err;
    }

    if (ftruncate(pool_fd, size) == -1) {
        LOG_ERRNO("failed to truncate SHM pool");
        goto err;
    }

    if (!can_punch_hole_initialized) {
        can_punch_hole_initialized = true;
        can_punch_hole = fallocate(
            pool_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 1) == 0;
    }

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
            .fd = pool_fd,
            .mmap_size = size,
            .offset = 0}
            )
        );

    struct buffer *ret = &tll_back(buffers);
    if (!instantiate_offset(shm, ret, 0))
        goto err;
    return ret;

err:
    if (pool_fd != -1)
        close(pool_fd);

    /* We don't handle this */
    abort();
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

bool
shm_scroll(struct wl_shm *shm, struct buffer *buf, int rows)
{
    assert(buf->busy);
    assert(buf->pix);
    assert(buf->wl_buf);
    assert(buf->real_mmapped);
    assert(buf->fd >= 0);

    if (!can_punch_hole)
        return false;

    LOG_DBG("scrolling %d rows (%d bytes)", rows, rows * buf->stride);

    assert(rows > 0);
    assert(rows * buf->stride < buf->size);
    const size_t new_offset = buf->offset + rows * buf->stride;

#if TIME_SCROLL
    struct timeval time0;
    gettimeofday(&time0, NULL);
#endif

    /* Increase file size */
    if (ftruncate(buf->fd, new_offset + buf->size) < 0) {
        LOG_ERRNO("failed increase memfd size from %zu -> %zu",
                  buf->offset + buf->size, new_offset + buf->size);
        return false;
    }

#if TIME_SCROLL
    struct timeval time1;
    gettimeofday(&time1, NULL);

    struct timeval tot;
    timersub(&time1, &time0, &tot);
    LOG_INFO("fallocate: %lds %ldus", tot.tv_sec, tot.tv_usec);
#endif

    /* Destroy old objects (they point to the old offset) */
    pixman_image_unref(buf->pix);
    wl_buffer_destroy(buf->wl_buf);
    munmap(buf->real_mmapped, buf->mmap_size);

    buf->pix = NULL;
    buf->wl_buf = NULL;
    buf->real_mmapped = buf->mmapped = NULL;

    /* Free unused memory */
    if (fallocate(buf->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, new_offset) < 0) {
        LOG_ERRNO(
            "fallocate(FALLOC_FL_PUNCH_HOLE) not "
            "supported: expect lower performance");
        abort();
    }

#if TIME_SCROLL
    struct timeval time2;
    gettimeofday(&time2, NULL);
    timersub(&time2, &time1, &tot);
    LOG_INFO("PUNCH HOLE: %lds %ldus", tot.tv_sec, tot.tv_usec);
#endif

    return instantiate_offset(shm, buf, new_offset);
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
