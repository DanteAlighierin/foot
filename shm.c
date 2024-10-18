#include "shm.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <pixman.h>

#include <fcft/stride.h>
#include <tllist.h>

#define LOG_MODULE "shm"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "macros.h"
#include "xmalloc.h"

#if !defined(MAP_UNINITIALIZED)
 #define MAP_UNINITIALIZED 0
#endif

#if !defined(MFD_NOEXEC_SEAL)
 #define MFD_NOEXEC_SEAL 0
#endif

#define TIME_SCROLL 0

#define FORCED_DOUBLE_BUFFERING 0

/*
 * Maximum memfd size allowed.
 *
 * On 64-bit, we could in theory use up to 2GB (wk_shm_create_pool()
 * is limited to int32_t), since we never mmap() the entire region.
 *
 * The compositor is different matter - it needs to mmap() the entire
 * range, and *keep* the mapping for as long as is has buffers
 * referencing it (thus - always). And if we open multiple terminals,
 * then the required address space multiples...
 *
 * That said, 128TB (the total amount of available user address space
 * on 64-bit) is *a lot*; we can fit 67108864 2GB memfds into
 * that. But, let's be conservative for now.
 *
 * On 32-bit the available address space is too small and SHM
 * scrolling is disabled.
 *
 * Note: this is the _default_ size. It can be overridden by calling
 * shm_set_max_pool_size();
 */
static off_t max_pool_size = 512 * 1024 * 1024;

static bool can_punch_hole = false;
static bool can_punch_hole_initialized = false;

struct buffer_pool {
    int fd;                /* memfd */
    struct wl_shm_pool *wl_pool;

    void *real_mmapped;    /* Address returned from mmap */
    size_t mmap_size;      /* Size of mmap (>= size) */

    size_t ref_count;
};

struct buffer_chain;
struct buffer_private {
    struct buffer public;
    struct buffer_chain *chain;

    size_t ref_count;
    bool busy;                /* Owned by compositor */

    struct buffer_pool *pool;
    off_t offset;             /* Offset into memfd where data begins */
    size_t size;
    bool with_alpha;

    bool scrollable;
};

struct buffer_chain {
    tll(struct buffer_private *) bufs;
    struct wl_shm *shm;
    size_t pix_instances;
    bool scrollable;
};

static tll(struct buffer_private *) deferred;

#undef MEASURE_SHM_ALLOCS
#if defined(MEASURE_SHM_ALLOCS)
static size_t max_alloced = 0;
#endif

void
shm_set_max_pool_size(off_t _max_pool_size)
{
    max_pool_size = _max_pool_size;
}

static void
buffer_destroy_dont_close(struct buffer *buf)
{
    if (buf->pix != NULL) {
        for (size_t i = 0; i < buf->pix_instances; i++)
            if (buf->pix[i] != NULL)
                pixman_image_unref(buf->pix[i]);
    }
    if (buf->wl_buf != NULL)
        wl_buffer_destroy(buf->wl_buf);

    free(buf->pix);
    buf->pix = NULL;
    buf->wl_buf = NULL;
    buf->data = NULL;
}

static void
pool_unref(struct buffer_pool *pool)
{
    if (pool == NULL)
        return;

    xassert(pool->ref_count > 0);
    pool->ref_count--;

    if (pool->ref_count > 0)
        return;

    if (pool->real_mmapped != MAP_FAILED)
        munmap(pool->real_mmapped, pool->mmap_size);
    if (pool->wl_pool != NULL)
        wl_shm_pool_destroy(pool->wl_pool);
    if (pool->fd >= 0)
        close(pool->fd);

    pool->real_mmapped = MAP_FAILED;
    pool->wl_pool = NULL;
    pool->fd = -1;
    free(pool);
}

static void
buffer_destroy(struct buffer_private *buf)
{
    buffer_destroy_dont_close(&buf->public);
    pool_unref(buf->pool);
    buf->pool = NULL;

    for (size_t i = 0; i < buf->public.pix_instances; i++)
        pixman_region32_fini(&buf->public.dirty[i]);
    free(buf->public.dirty);
    free(buf);
}

static bool
buffer_unref_no_remove_from_chain(struct buffer_private *buf)
{
    xassert(buf->ref_count > 0);
    buf->ref_count--;

    if (buf->ref_count > 0)
        return false;

    if (buf->busy)
        tll_push_back(deferred, buf);
    else
        buffer_destroy(buf);
    return true;
}

void
shm_fini(void)
{
    LOG_DBG("deferred buffers: %zu", tll_length(deferred));

    tll_foreach(deferred, it) {
        buffer_destroy(it->item);
        tll_remove(deferred, it);
    }

#if defined(MEASURE_SHM_ALLOCS) && MEASURE_SHM_ALLOCS
    LOG_INFO("max total allocations was: %zu MB", max_alloced / 1024 / 1024);
#endif
}

static void
buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    struct buffer_private *buffer = data;

    xassert(buffer->public.wl_buf == wl_buffer);
    xassert(buffer->busy);
    buffer->busy = false;

    if (buffer->ref_count == 0) {
        bool found = false;
        tll_foreach(deferred, it) {
            if (it->item == buffer) {
                found = true;
                tll_remove(deferred, it);
                break;
            }
        }

        buffer_destroy(buffer);

        xassert(found);
        if (!found)
            LOG_WARN("deferred delete: buffer not on the 'deferred' list");
    }
}

static const struct wl_buffer_listener buffer_listener = {
    .release = &buffer_release,
};

#if __SIZEOF_POINTER__ == 8
static size_t
page_size(void)
{
    static size_t size = 0;
    if (size == 0) {
        long n = sysconf(_SC_PAGE_SIZE);
        if (n <= 0) {
            LOG_ERRNO("failed to get page size");
            size = 4096;
        } else {
            size = (size_t)n;
        }
    }
    xassert(size > 0);
    return size;
}
#endif

static bool
instantiate_offset(struct buffer_private *buf, off_t new_offset)
{
    xassert(buf->public.data == NULL);
    xassert(buf->public.pix == NULL);
    xassert(buf->public.wl_buf == NULL);
    xassert(buf->pool != NULL);

    const struct buffer_pool *pool = buf->pool;

    void *mmapped = MAP_FAILED;
    struct wl_buffer *wl_buf = NULL;
    pixman_image_t **pix = xcalloc(buf->public.pix_instances, sizeof(pix[0]));

    mmapped = (uint8_t *)pool->real_mmapped + new_offset;

    wl_buf = wl_shm_pool_create_buffer(
        pool->wl_pool, new_offset,
        buf->public.width, buf->public.height, buf->public.stride,
        buf->with_alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);

    if (wl_buf == NULL) {
        LOG_ERR("failed to create SHM buffer");
        goto err;
    }

    /* One pixman image for each worker thread (do we really need multiple?) */
    for (size_t i = 0; i < buf->public.pix_instances; i++) {
        pix[i] = pixman_image_create_bits_no_clear(
            buf->with_alpha ? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8,
            buf->public.width, buf->public.height,
            (uint32_t *)mmapped, buf->public.stride);
        if (pix[i] == NULL) {
            LOG_ERR("failed to create pixman image");
            goto err;
        }
    }

    buf->public.data = mmapped;
    buf->public.wl_buf = wl_buf;
    buf->public.pix = pix;
    buf->offset = new_offset;

    wl_buffer_add_listener(wl_buf, &buffer_listener, buf);
    return true;

err:
    if (pix != NULL) {
        for (size_t i = 0; i < buf->public.pix_instances; i++)
            if (pix[i] != NULL)
                pixman_image_unref(pix[i]);
    }
    free(pix);
    if (wl_buf != NULL)
        wl_buffer_destroy(wl_buf);

    abort();
    return false;
}

static void NOINLINE
get_new_buffers(struct buffer_chain *chain, size_t count,
                int widths[static count], int heights[static count],
                struct buffer *bufs[static count], bool with_alpha,
                bool immediate_purge)
{
    xassert(count == 1 || !chain->scrollable);
    /*
     * No existing buffer available. Create a new one by:
     *
     * 1. open a memory backed "file" with memfd_create()
     * 2. mmap() the memory file, to be used by the pixman image
     * 3. create a wayland shm buffer for the same memory file
     *
     * The pixman image and the wayland buffer are now sharing memory.
     */

    int stride[count];
    int sizes[count];

    size_t total_size = 0;
    for (size_t i = 0; i < count; i++) {
        stride[i] = stride_for_format_and_width(
            with_alpha ? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8, widths[i]);
        sizes[i] = stride[i] * heights[i];
        total_size += sizes[i];
    }
    if (total_size == 0)
        return;

    int pool_fd = -1;

    void *real_mmapped = MAP_FAILED;
    struct wl_shm_pool *wl_pool = NULL;
    struct buffer_pool *pool = NULL;

    /* Backing memory for SHM */
#if defined(MEMFD_CREATE)
    /*
     * Older kernels reject MFD_NOEXEC_SEAL with EINVAL. Try first
     * *with* it, and if that fails, try again *without* it.
     */
    errno = 0;
    pool_fd = memfd_create(
        "foot-wayland-shm-buffer-pool",
        MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);

    if (pool_fd < 0 && errno == EINVAL && MFD_NOEXEC_SEAL != 0) {
        pool_fd = memfd_create(
            "foot-wayland-shm-buffer-pool", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    }

#elif defined(__FreeBSD__)
    // memfd_create on FreeBSD 13 is SHM_ANON without sealing support
    pool_fd = shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600);
#else
    char name[] = "/tmp/foot-wayland-shm-buffer-pool-XXXXXX";
    pool_fd = mkostemp(name, O_CLOEXEC);
    unlink(name);
#endif
    if (pool_fd == -1) {
        LOG_ERRNO("failed to create SHM backing memory file");
        goto err;
    }

#if __SIZEOF_POINTER__ == 8
    off_t offset = chain->scrollable && max_pool_size > 0
        ? (max_pool_size / 4) & ~(page_size() - 1)
        : 0;
    off_t memfd_size = chain->scrollable && max_pool_size > 0
        ? max_pool_size
        : total_size;
#else
    off_t offset = 0;
    off_t memfd_size = total_size;
#endif

    xassert(chain->scrollable || (offset == 0 && memfd_size == total_size));

    LOG_DBG("memfd-size: %lu, initial offset: %lu", memfd_size, offset);

    if (ftruncate(pool_fd, memfd_size) == -1) {
        LOG_ERRNO("failed to set size of SHM backing memory file");
        goto err;
    }

    if (!can_punch_hole_initialized) {
        can_punch_hole_initialized = true;
#if __SIZEOF_POINTER__ == 8 && defined(FALLOC_FL_PUNCH_HOLE)
        can_punch_hole = fallocate(
            pool_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 1) == 0;

        if (!can_punch_hole) {
            LOG_WARN(
                "fallocate(FALLOC_FL_PUNCH_HOLE) not "
                "supported (%s): expect lower performance", strerror(errno));
        }
#else
        /* This is mostly to make sure we skip the warning issued
         * above */
        can_punch_hole = false;
#endif
    }

    if (chain->scrollable && !can_punch_hole) {
        offset = 0;
        memfd_size = total_size;
        chain->scrollable = false;

        if (ftruncate(pool_fd, memfd_size) < 0) {
            LOG_ERRNO("failed to set size of SHM backing memory file");
            goto err;
        }
    }

    real_mmapped = mmap(
        NULL, memfd_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_UNINITIALIZED, pool_fd, 0);

    if (real_mmapped == MAP_FAILED) {
        LOG_ERRNO("failed to mmap SHM backing memory file");
        goto err;
    }

#if defined(MEMFD_CREATE)
    /* Seal file - we no longer allow any kind of resizing */
    /* TODO: wayland mmaps(PROT_WRITE), for some unknown reason, hence we cannot use F_SEAL_FUTURE_WRITE */
    if (fcntl(pool_fd, F_ADD_SEALS,
              F_SEAL_GROW | F_SEAL_SHRINK | /*F_SEAL_FUTURE_WRITE |*/ F_SEAL_SEAL) < 0)
    {
        LOG_ERRNO("failed to seal SHM backing memory file");
        /* This is not a fatal error */
    }
#endif

    wl_pool = wl_shm_create_pool(chain->shm, pool_fd, memfd_size);
    if (wl_pool == NULL) {
        LOG_ERR("failed to create SHM pool");
        goto err;
    }

    pool = xmalloc(sizeof(*pool));
    if (pool == NULL) {
        LOG_ERRNO("failed to allocate buffer pool");
        goto err;
    }

    *pool = (struct buffer_pool){
        .fd = pool_fd,
        .wl_pool = wl_pool,
        .real_mmapped = real_mmapped,
        .mmap_size = memfd_size,
        .ref_count = 0,
    };

    for (size_t i = 0; i < count; i++) {
        if (sizes[i] == 0) {
            bufs[i] = NULL;
            continue;
        }

        /* Push to list of available buffers, but marked as 'busy' */
        struct buffer_private *buf = xmalloc(sizeof(*buf));
        *buf = (struct buffer_private){
            .public = {
                .width = widths[i],
                .height = heights[i],
                .stride = stride[i],
                .pix_instances = chain->pix_instances,
                .age = 1234,  /* Force a full repaint */
            },
            .chain = chain,
            .ref_count = immediate_purge ? 0 : 1,
            .busy = true,
            .with_alpha = with_alpha,
            .pool = pool,
            .offset = 0,
            .size = sizes[i],
            .scrollable = chain->scrollable,
        };

        if (!instantiate_offset(buf, offset)) {
            free(buf);
            goto err;
        }

        if (immediate_purge)
            tll_push_front(deferred, buf);
        else
            tll_push_front(chain->bufs, buf);

        buf->public.dirty = xmalloc(
            chain->pix_instances * sizeof(buf->public.dirty[0]));

        for (size_t j = 0; j < chain->pix_instances; j++)
            pixman_region32_init(&buf->public.dirty[j]);

        pool->ref_count++;
        offset += buf->size;
        bufs[i] = &buf->public;
    }

#if defined(MEASURE_SHM_ALLOCS) && MEASURE_SHM_ALLOCS
    {
        size_t currently_alloced = 0;
        tll_foreach(buffers, it)
            currently_alloced += it->item.size;
        if (currently_alloced > max_alloced)
            max_alloced = currently_alloced;
    }
#endif

    if (!(bufs[0] && shm_can_scroll(bufs[0]))) {
        /* We only need to keep the pool FD open if we're going to SHM
         * scroll it */
        close(pool_fd);
        pool->fd = -1;
    }

    return;

err:
    pool_unref(pool);
    if (wl_pool != NULL)
        wl_shm_pool_destroy(wl_pool);
    if (real_mmapped != MAP_FAILED)
        munmap(real_mmapped, memfd_size);
    if (pool_fd != -1)
        close(pool_fd);

    /* We don't handle this */
    abort();
}

void
shm_did_not_use_buf(struct buffer *_buf)
{
    struct buffer_private *buf = (struct buffer_private *)_buf;
    buf->busy = false;
}

void
shm_get_many(struct buffer_chain *chain, size_t count,
             int widths[static count], int heights[static count],
             struct buffer *bufs[static count], bool with_alpha)
{
    get_new_buffers(chain, count, widths, heights, bufs, with_alpha, true);
}

struct buffer *
shm_get_buffer(struct buffer_chain *chain, int width, int height, bool with_alpha)
{
    LOG_DBG(
        "chain=%p: looking for a reusable %dx%d buffer "
        "among %zu potential buffers",
        (void *)chain, width, height, tll_length(chain->bufs));

    struct buffer_private *cached = NULL;
    tll_foreach(chain->bufs, it) {
        struct buffer_private *buf = it->item;

        if (buf->public.width != width || buf->public.height != height ||
            with_alpha != buf->with_alpha)
        {
            LOG_DBG("purging mismatching buffer %p", (void *)buf);
            if (buffer_unref_no_remove_from_chain(buf))
                tll_remove(chain->bufs, it);
            continue;
        }

        if (buf->busy)
            buf->public.age++;
        else
#if FORCED_DOUBLE_BUFFERING
            if (buf->public.age == 0)
                buf->public.age++;
            else
#endif
            {
                if (cached == NULL)
                    cached = buf;
                else {
                    /* We have multiple buffers eligible for
                     * reuse. Pick the "youngest" one, and mark the
                     * other one for purging */
                    if (buf->public.age < cached->public.age) {
                        shm_unref(&cached->public);
                        cached = buf;
                    } else {
                        /*
                         * TODO: I think we _can_ use shm_unref()
                         * here...
                         *
                         * shm_unref() may remove 'it', but that
                         * should be safe; "our" tll_foreach() already
                         * holds the next pointer.
                         */
                        if (buffer_unref_no_remove_from_chain(buf))
                            tll_remove(chain->bufs, it);
                    }
                }
            }
    }

    if (cached != NULL) {
        LOG_DBG("re-using buffer %p from cache", (void *)cached);
        cached->busy = true;
        for (size_t i = 0; i < cached->public.pix_instances; i++)
            pixman_region32_clear(&cached->public.dirty[i]);
        xassert(cached->public.pix_instances == chain->pix_instances);
        return &cached->public;
    }

    struct buffer *ret;
    get_new_buffers(chain, 1, &width, &height, &ret, with_alpha, false);
    return ret;
}

bool
shm_can_scroll(const struct buffer *_buf)
{
#if __SIZEOF_POINTER__ == 8
    const struct buffer_private *buf = (const struct buffer_private *)_buf;
    return can_punch_hole && max_pool_size > 0 && buf->scrollable;
#else
    /* Not enough virtual address space in 32-bit */
    return false;
#endif
}

#if __SIZEOF_POINTER__ == 8 && defined(FALLOC_FL_PUNCH_HOLE)
static bool
wrap_buffer(struct buffer_private *buf, off_t new_offset)
{
    struct buffer_pool *pool = buf->pool;
    xassert(pool->ref_count == 1);

    /* We don't allow overlapping offsets */
    off_t UNUSED diff = new_offset < buf->offset
        ? buf->offset - new_offset
        : new_offset - buf->offset;
    xassert(diff > buf->size);

    memcpy((uint8_t *)pool->real_mmapped + new_offset,
           buf->public.data,
           buf->size);

    off_t trim_ofs, trim_len;
    if (new_offset > buf->offset) {
        /* Trim everything *before* the new offset */
        trim_ofs = 0;
        trim_len = new_offset;
    } else {
        /* Trim everything *after* the new buffer location */
        trim_ofs = new_offset + buf->size;
        trim_len = pool->mmap_size - trim_ofs;
    }

    if (fallocate(
            pool->fd,
            FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
            trim_ofs, trim_len) < 0)
    {
        LOG_ERRNO("failed to trim SHM backing memory file");
        return false;
    }

    /* Re-instantiate pixman+wl_buffer+raw pointersw */
    buffer_destroy_dont_close(&buf->public);
    return instantiate_offset(buf, new_offset);
}

static bool
shm_scroll_forward(struct buffer_private *buf, int rows,
                   int top_margin, int top_keep_rows,
                   int bottom_margin, int bottom_keep_rows)
{
    struct buffer_pool *pool = buf->pool;

    xassert(can_punch_hole);
    xassert(buf->busy);
    xassert(buf->public.pix != NULL);
    xassert(buf->public.wl_buf != NULL);
    xassert(pool != NULL);
    xassert(pool->ref_count == 1);
    xassert(pool->fd >= 0);

    LOG_DBG("scrolling %d rows (%d bytes)", rows, rows * buf->public.stride);

    const off_t diff = rows * buf->public.stride;
    xassert(rows > 0);
    xassert(diff < buf->size);

    if (buf->offset + diff + buf->size > max_pool_size) {
        LOG_DBG("memfd offset wrap around");
        if (!wrap_buffer(buf, 0))
            goto err;
    }

    off_t new_offset = buf->offset + diff;
    xassert(new_offset > buf->offset);
    xassert(new_offset + buf->size <= max_pool_size);

#if TIME_SCROLL
    struct timespec tot;
    struct timespec time1;
    clock_gettime(CLOCK_MONOTONIC, &time1);

    struct timespec time2 = time1;
#endif

    if (top_keep_rows > 0) {
        /* Copy current 'top' region to its new location */
        const int stride = buf->public.stride;
        uint8_t *base = buf->public.data;

        memmove(
            base + (top_margin + rows) * stride,
            base + (top_margin + 0) * stride,
            top_keep_rows * stride);

#if TIME_SCROLL
        clock_gettime(CLOCK_MONOTONIC, &time2);
        timespec_sub(&time2, &time1, &tot);
        LOG_INFO("memmove (top region): %lds %ldns",
                 (long)tot.tv_sec, tot.tv_nsec);
#endif
    }

    /* Destroy old objects (they point to the old offset) */
    buffer_destroy_dont_close(&buf->public);

    /* Free unused memory - everything up until the new offset */
    const off_t trim_ofs = 0;
    const off_t trim_len = new_offset;

    if (fallocate(
            pool->fd,
            FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
            trim_ofs, trim_len) < 0)
    {
        LOG_ERRNO("failed to trim SHM backing memory file");
        goto err;
    }

#if TIME_SCROLL
    struct timespec time3;
    clock_gettime(CLOCK_MONOTONIC, &time3);
    timespec_sub(&time3, &time2, &tot);
    LOG_INFO("PUNCH HOLE: %lds %ldns", (long)tot.tv_sec, tot.tv_nsec);
#endif

    /* Re-instantiate pixman+wl_buffer+raw pointersw */
    bool ret = instantiate_offset(buf, new_offset);

#if TIME_SCROLL
    struct timespec time4;
    clock_gettime(CLOCK_MONOTONIC, &time4);
    timespec_sub(&time4, &time3, &tot);
    LOG_INFO("instantiate offset: %lds %ldns", (long)tot.tv_sec, tot.tv_nsec);
#endif

    if (ret && bottom_keep_rows > 0) {
        /* Copy 'bottom' region to its new location */
        const size_t size = buf->size;
        const int stride = buf->public.stride;
        uint8_t *base = buf->public.data;

        memmove(
            base + size - (bottom_margin + bottom_keep_rows) * stride,
            base + size - (bottom_margin + rows + bottom_keep_rows) * stride,
            bottom_keep_rows * stride);

#if TIME_SCROLL
        struct timespec time5;
        clock_gettime(CLOCK_MONOTONIC, &time5);

        timespec_sub(&time5, &time4, &tot);
        LOG_INFO("memmove (bottom region): %lds %ldns",
                 (long)tot.tv_sec, tot.tv_nsec);
#endif
    }

    return ret;

err:
    abort();
    return false;
}

static bool
shm_scroll_reverse(struct buffer_private *buf, int rows,
                   int top_margin, int top_keep_rows,
                   int bottom_margin, int bottom_keep_rows)
{
    xassert(rows > 0);

    struct buffer_pool *pool = buf->pool;
    xassert(pool->ref_count == 1);

    const off_t diff = rows * buf->public.stride;
    if (diff > buf->offset) {
        LOG_DBG("memfd offset reverse wrap-around");
        if (!wrap_buffer(buf, (max_pool_size - buf->size) & ~(page_size() - 1)))
            goto err;
    }

    off_t new_offset = buf->offset - diff;
    xassert(new_offset < buf->offset);
    xassert(new_offset <= max_pool_size);

#if TIME_SCROLL
    struct timespec time0;
    clock_gettime(CLOCK_MONOTONIC, &time0);

    struct timespec tot;
    struct timespec time1 = time0;
#endif

    if (bottom_keep_rows > 0) {
        /* Copy 'bottom' region to its new location */
        const size_t size = buf->size;
        const int stride = buf->public.stride;
        uint8_t *base = buf->public.data;

        memmove(
            base + size - (bottom_margin + rows + bottom_keep_rows) * stride,
            base + size - (bottom_margin + bottom_keep_rows) * stride,
            bottom_keep_rows * stride);

#if TIME_SCROLL
        clock_gettime(CLOCK_MONOTONIC, &time1);
        timespec_sub(&time1, &time0, &tot);
        LOG_INFO("memmove (bottom region): %lds %ldns",
                 (long)tot.tv_sec, tot.tv_nsec);
#endif
    }

    /* Destroy old objects (they point to the old offset) */
    buffer_destroy_dont_close(&buf->public);

    /* Free unused memory - everything after the relocated buffer */
    const off_t trim_ofs = new_offset + buf->size;
    const off_t trim_len = pool->mmap_size - trim_ofs;

    if (fallocate(
            pool->fd,
            FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
            trim_ofs, trim_len) < 0)
    {
        LOG_ERRNO("failed to trim SHM backing memory");
        goto err;
    }
#if TIME_SCROLL
    struct timespec time2;
    clock_gettime(CLOCK_MONOTONIC, &time2);
    timespec_sub(&time2, &time1, &tot);
    LOG_INFO("fallocate: %lds %ldns", (long)tot.tv_sec, tot.tv_nsec);
#endif

    /* Re-instantiate pixman+wl_buffer+raw pointers */
    bool ret = instantiate_offset(buf, new_offset);

#if TIME_SCROLL
    struct timespec time3;
    clock_gettime(CLOCK_MONOTONIC, &time3);
    timespec_sub(&time3, &time2, &tot);
    LOG_INFO("instantiate offset: %lds %ldns", (long)tot.tv_sec, tot.tv_nsec);
#endif

    if (ret && top_keep_rows > 0) {
        /* Copy current 'top' region to its new location */
        const int stride = buf->public.stride;
        uint8_t *base = buf->public.data;

        memmove(
            base + (top_margin + 0) * stride,
            base + (top_margin + rows) * stride,
            top_keep_rows * stride);

#if TIME_SCROLL
        struct timespec time4;
        clock_gettime(CLOCK_MONOTONIC, &time4);
        timespec_sub(&time4, &time3, &tot);
        LOG_INFO("memmove (top region): %lds %ldns",
                 (long)tot.tv_sec, tot.tv_nsec);
#endif
    }

    return ret;

err:
    abort();
    return false;
}
#endif /* FALLOC_FL_PUNCH_HOLE */

bool
shm_scroll(struct buffer *_buf, int rows,
           int top_margin, int top_keep_rows,
           int bottom_margin, int bottom_keep_rows)
{
#if __SIZEOF_POINTER__ == 8 && defined(FALLOC_FL_PUNCH_HOLE)
    if (!shm_can_scroll(_buf))
        return false;

    struct buffer_private *buf = (struct buffer_private *)_buf;

    xassert(rows != 0);
    return rows > 0
        ? shm_scroll_forward(buf, rows, top_margin, top_keep_rows, bottom_margin, bottom_keep_rows)
        : shm_scroll_reverse(buf, -rows, top_margin, top_keep_rows, bottom_margin, bottom_keep_rows);
#else
    return false;
#endif
}

void
shm_purge(struct buffer_chain *chain)
{
    LOG_DBG("chain: %p: purging all buffers", (void *)chain);

    /* Purge old buffers associated with this cookie */
    tll_foreach(chain->bufs, it) {
        if (buffer_unref_no_remove_from_chain(it->item))
            tll_remove(chain->bufs, it);
    }
}

void
shm_addref(struct buffer *_buf)
{
    struct buffer_private *buf = (struct buffer_private *)_buf;
    buf->ref_count++;
}

void
shm_unref(struct buffer *_buf)
{
    if (_buf == NULL)
        return;

    struct buffer_private *buf = (struct buffer_private *)_buf;
    struct buffer_chain *chain = buf->chain;

    tll_foreach(chain->bufs, it) {
        if (it->item != buf)
            continue;

        if (buffer_unref_no_remove_from_chain(buf))
            tll_remove(chain->bufs, it);
        break;
    }
}

struct buffer_chain *
shm_chain_new(struct wl_shm *shm, bool scrollable, size_t pix_instances)
{
    struct buffer_chain *chain = xmalloc(sizeof(*chain));
    *chain = (struct buffer_chain){
        .bufs = tll_init(),
        .shm = shm,
        .pix_instances = pix_instances,
        .scrollable = scrollable,
    };
    return chain;
}

void
shm_chain_free(struct buffer_chain *chain)
{
    if (chain == NULL)
        return;

    shm_purge(chain);

    if (tll_length(chain->bufs) > 0) {
        BUG("chain=%p: there are buffers remaining; "
            "is there a missing call to shm_unref()?", (void *)chain);
    }

    free(chain);
}
