#include "shm.h"

#include <unistd.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <linux/memfd.h>

#include <pixman.h>

#define LOG_MODULE "shm"
#include "log.h"
#include "tllist.h"

static tll(struct buffer) buffers;

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
shm_get_buffer(struct wl_shm *shm, int width, int height, size_t copies)
{
    assert(copies >= 1);

    tll_foreach(buffers, it) {
        if (it->item.width != width || it->item.height != height)
            continue;

        if (!it->item.busy) {
            it->item.busy = true;
            return &it->item;
        }
    }

    /*
     * No existing buffer available. Create a new one by:
     *
     * 1. open a memory backed "file" with memfd_create()
     * 2. mmap() the memory file, to be used by the cairo surface
     * 3. create a wayland shm buffer for the same memory file
     *
     * The cairo surface and the wayland buffer are now sharing
     * memory.
     */

    int pool_fd = -1;
    void *mmapped = NULL;
    size_t size = 0;

    struct wl_shm_pool *pool = NULL;
    struct wl_buffer *buf = NULL;

    cairo_surface_t **cairo_surface = NULL;
    cairo_t **cairo = NULL;
    pixman_image_t **pix = NULL;

    /* Backing memory for SHM */
    pool_fd = memfd_create("f00sel-wayland-shm-buffer-pool", MFD_CLOEXEC);
    if (pool_fd == -1) {
        LOG_ERRNO("failed to create SHM backing memory file");
        goto err;
    }

    /* Total size */
    const uint32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    size = stride * height;
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

    /* Create a cairo surface around the mmapped memory */
    cairo_surface = calloc(copies, sizeof(cairo_surface[0]));
    cairo = calloc(copies, sizeof(cairo[0]));
    pix = calloc(copies, sizeof(pix[0]));

    for (size_t i = 0; i < copies; i++) {
        cairo_surface[i] = cairo_image_surface_create_for_data(
            mmapped, CAIRO_FORMAT_ARGB32, width, height, stride);

        if (cairo_surface_status(cairo_surface[i]) != CAIRO_STATUS_SUCCESS) {
            LOG_ERR("failed to create cairo surface: %s",
                    cairo_status_to_string(cairo_surface_status(cairo_surface[i])));
            goto err;
        }

        cairo[i] = cairo_create(cairo_surface[i]);
        if (cairo_status(cairo[i]) != CAIRO_STATUS_SUCCESS) {
            LOG_ERR("failed to create cairo context: %s",
                    cairo_status_to_string(cairo_status(cairo[i])));
            goto err;
        }

        pix[i] = pixman_image_create_bits_no_clear(
            PIXMAN_a8r8g8b8, width, height, (uint32_t *)mmapped, stride);

        if (pix[i] == NULL) {
            LOG_ERR("failed to create pixman image");
            goto err;
        }
    }

    /* Push to list of available buffers, but marked as 'busy' */
    tll_push_back(
        buffers,
        ((struct buffer){
            .width = width,
            .height = height,
            .stride = stride,
            .busy = true,
            .size = size,
            .mmapped = mmapped,
            .wl_buf = buf,
            .copies = copies,
            .cairo_surface = cairo_surface,
            .cairo = cairo,
            .pix = pix}
            )
        );

    struct buffer *ret = &tll_back(buffers);
    wl_buffer_add_listener(ret->wl_buf, &buffer_listener, ret);
    return ret;

err:
    if (cairo != NULL) {
        for (size_t i = 0; i < copies; i++)
            if (cairo[i] != NULL)
                cairo_destroy(cairo[i]);
        free(cairo);
    }
    if (cairo_surface != NULL) {
        for (size_t i = 0; i < copies; i++)
            if (cairo_surface[i] != NULL)
                cairo_surface_destroy(cairo_surface[i]);
        free(cairo_surface);
    }
    if (pix != NULL) {
        for (size_t i = 0; i < copies; i++)
            if (pix[i] != NULL)
                pixman_image_unref(pix[i]);
        free(pix);
    }
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
        struct buffer *buf = &it->item;

        if (buf->cairo != NULL) {
            for (size_t i = 0; i < buf->copies; i++)
                if (buf->cairo[i] != NULL)
                    cairo_destroy(buf->cairo[i]);
            free(buf->cairo);
        }
        if (buf->cairo_surface != NULL) {
            for (size_t i = 0; i < buf->copies; i++)
                if (buf->cairo_surface[i] != NULL)
                    cairo_surface_destroy(buf->cairo_surface[i]);
            free(buf->cairo_surface);
        }
        if (buf->pix != NULL) {
            for (size_t i = 0; i < buf->copies; i++)
                if (buf->pix[i] != NULL)
                    pixman_image_unref(buf->pix[i]);
            free(buf->pix);
        }
        wl_buffer_destroy(buf->wl_buf);
        munmap(buf->mmapped, buf->size);

        tll_remove(buffers, it);
    }
}
