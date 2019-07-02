#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>

#include <sys/ioctl.h>
//#include <termios.h>

#include <wayland-client.h>
#include <xdg-shell.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 1
#include "log.h"

#include "font.h"
#include "shm.h"
#include "slave.h"
#include "terminal.h"
#include "vt.h"
#include "input.h"
#include "grid.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

static const struct rgba default_foreground = {1.0, 1.0, 1.0, 1.0};
static const struct rgba default_background = {0.0, 0.0, 0.0, 1.0};

struct wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct xdg_wm_base *shell;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
};

struct glyph_cache {
    cairo_scaled_font_t *font;
    cairo_surface_t *surf[256];
    cairo_pattern_t *cache[256];
};

struct context {
    bool quit;

    struct glyph_cache fonts[4];
    cairo_font_extents_t fextents;

    int width;
    int height;

    struct wayland wl;
    //struct grid grid;
    struct terminal term;

    bool frame_is_scheduled;
};

static void frame_callback(
    void *data, struct wl_callback *wl_callback, uint32_t callback_data);

static const struct wl_callback_listener frame_listener = {
    .done = &frame_callback,
};

static struct glyph_cache *
attrs_to_font(struct context *c, const struct attributes *attrs)
{
    int idx = attrs->italic << 1 | attrs->bold;
    return &c->fonts[idx];
}

static void
grid_render_update(struct context *c, struct buffer *buf, const struct damage *dmg)
{
    LOG_DBG("damage: UPDATE: %d -> %d (offset = %d)",
            (dmg->range.start - c->term.grid->offset) % c->term.grid->size,
            (dmg->range.start - c->term.grid->offset) % c->term.grid->size + dmg->range.length,
        c->term.grid->offset);

    int start = dmg->range.start;
    int length = dmg->range.length;

    if (start < c->term.grid->offset) {
        int end = start + length;
        if (end >= c->term.grid->offset) {
            start = c->term.grid->offset;
            length = end - start;
        } else
            return;
    }

    const int cols = c->term.cols;

    for (int linear_cursor = start,
             row = ((start - c->term.grid->offset) % c->term.grid->size) / cols,
             col = ((start - c->term.grid->offset) % c->term.grid->size) % cols;
         linear_cursor < start + length;
         linear_cursor++,
             //col = (col + 1) % cols,
             col = col + 1 >= c->term.cols ? 0 : col + 1,
             row += col == 0 ? 1 : 0)
    {

        //LOG_DBG("UPDATE: %d (%dx%d)", linear_cursor, row, col);

        //assert(linear_cursor < c->term.grid->size);
        const struct cell *cell = &c->term.grid->cells[linear_cursor % c->term.grid->size];

        bool has_cursor = c->term.cursor.linear == linear_cursor - c->term.grid->offset;
        //bool has_cursor = false;

        int x = col * c->term.cell_width;
        int y = row * c->term.cell_height;
        int width = c->term.cell_width;
        int height = c->term.cell_height;

        struct rgba foreground = cell->attrs.have_foreground
            ? cell->attrs.foreground : c->term.foreground;
        struct rgba background = cell->attrs.have_background
            ? cell->attrs.background : c->term.background;

        if (has_cursor) {
            struct rgba swap = foreground;
            foreground = background;
            background = swap;
        }

        if (cell->attrs.reverse) {
            struct rgba swap = foreground;
            foreground = background;
            background = swap;
        }

        /* Background */
        cairo_set_source_rgba(
            buf->cairo, background.r, background.g, background.b, background.a);
        cairo_rectangle(buf->cairo, x, y, width, height);
        cairo_fill(buf->cairo);

        if (cell->c[0] == '\0' || cell->c[0] == ' ')
            continue;

        if (cell->attrs.conceal)
            continue;

        struct glyph_cache *cache = attrs_to_font(c, &cell->attrs);

        cairo_set_source_rgba(
            buf->cairo, foreground.r, foreground.g, foreground.b, foreground.a);

        if (strlen(cell->c) == 1 && cache->cache[(int)cell->c[0]] != NULL) {
            cairo_matrix_t matrix;
            cairo_matrix_init_translate(&matrix, -x, -y);

            cairo_pattern_t *pat = cache->cache[(int)cell->c[0]];
            cairo_pattern_set_matrix(pat, &matrix);

#if 1
            cairo_pattern_set_matrix(pat, &matrix);
            cairo_mask(buf->cairo, pat);
#else  /* TODO: blit image instead - but doesn't (yet) handle colors */
            cairo_set_source(buf->cairo, pat);
            cairo_rectangle(buf->cairo, x, y, width, height);
            cairo_fill(buf->cairo);
#endif
        } else {
            cairo_glyph_t *glyphs = NULL;
            int num_glyphs = 0;

            cairo_status_t status = cairo_scaled_font_text_to_glyphs(
                cache->font, x, y + c->fextents.ascent,
                cell->c, strlen(cell->c), &glyphs, &num_glyphs,
                NULL, NULL, NULL);

            if (status != CAIRO_STATUS_SUCCESS) {
                if (glyphs != NULL)
                    cairo_glyph_free(glyphs);
                continue;
            }

            cairo_set_scaled_font(buf->cairo, cache->font);
            cairo_show_glyphs(buf->cairo, glyphs, num_glyphs);
            cairo_glyph_free(glyphs);
        }
    }

    wl_surface_damage_buffer(
        c->wl.surface,
        0, ((dmg->range.start - c->term.grid->offset) / cols) * c->term.cell_height,
        buf->width, (dmg->range.length + cols - 1) / cols * c->term.cell_height);
}

static void
grid_render_erase(struct context *c, struct buffer *buf, const struct damage *dmg)
{
    LOG_DBG("damage: ERASE: %d -> %d (offset = %d)",
            (dmg->range.start - c->term.grid->offset) % c->term.grid->size,
            (dmg->range.start - c->term.grid->offset) % c->term.grid->size + dmg->range.length,
            c->term.grid->offset);

    assert(dmg->range.start >= c->term.grid->offset);

    cairo_set_source_rgba(
        buf->cairo, default_background.r, default_background.g,
        default_background.b, default_background.a);

    const int cols = c->term.cols;

    int start = (dmg->range.start - c->term.grid->offset) % c->term.grid->size;
    int left = dmg->range.length;

    int row = start / cols;
    int col = start % cols;

    /* Partial initial line */
    if (col != 0) {
        int cell_count = min(left, cols - col);

        int x = col * c->term.cell_width;
        int y = row * c->term.cell_height;
        int width = cell_count * c->term.cell_width;
        int height = c->term.cell_height;

        cairo_rectangle(buf->cairo, x, y, width, height);
        cairo_fill(buf->cairo);
        wl_surface_damage_buffer(c->wl.surface, x, y, width, height);

        start += cell_count;
        left -= cell_count;

        row = start / cols;
        col = start % cols;
    }

    assert(left == 0 || col == 0);

    /* One or more full lines left */
    if (left >= cols) {
        int line_count = left / cols;

        int x = 0;
        int y = row * c->term.cell_height;
        int width = buf->width;
        int height = line_count * c->term.cell_height;

        cairo_rectangle(buf->cairo, x, y, width, height);
        cairo_fill(buf->cairo);
        wl_surface_damage_buffer(c->wl.surface, x, y, width, height);

        start += line_count * cols;
        left -= line_count * cols;

        row += line_count;
        col = 0;
    }

    assert(left == 0 || col == 0);
    assert(left < cols);

    /* Partial last line */
    if (left > 0) {
        int x = 0;
        int y = row * c->term.cell_height;
        int width = left * c->term.cell_width;
        int height = c->term.cell_height;

        cairo_rectangle(buf->cairo, x, y, width, height);
        cairo_fill(buf->cairo);
        wl_surface_damage_buffer(c->wl.surface, x, y, width, height);
    }
}

static void
grid_render_scroll(struct context *c, struct buffer *buf,
                   const struct damage *dmg)
{
    int dst_y = (dmg->scroll.region.start + 0) * c->term.cell_height;
    int src_y = (dmg->scroll.region.start + dmg->scroll.lines) * c->term.cell_height;
    int width = buf->width;
    int height = (dmg->scroll.region.end - dmg->scroll.region.start - dmg->scroll.lines) * c->term.cell_height;

    const uint32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

    LOG_DBG("damage: SCROLL: %d-%d by %d lines (dst-y: %d, src-y: %d, "
            "height: %d, stride: %d, mmap-size: %zu)",
            dmg->scroll.region.start, dmg->scroll.region.end,
            dmg->scroll.lines,
            dst_y, src_y, height, stride,
            buf->size);

    if (height > 0) {
        cairo_surface_flush(buf->cairo_surface);
        uint8_t *raw = cairo_image_surface_get_data(buf->cairo_surface);

        memmove(raw + dst_y * stride, raw + src_y * stride, height * stride);
        cairo_surface_mark_dirty(buf->cairo_surface);

        wl_surface_damage_buffer(c->wl.surface, 0, dst_y, width, height);
    }

    const int cols = c->term.cols;

    struct damage erase = {
        .type = DAMAGE_ERASE,
        .range = {
            .start = c->term.grid->offset + max(dmg->scroll.region.end - dmg->scroll.lines,
                         dmg->scroll.region.start) * cols,
            .length = min(dmg->scroll.region.end - dmg->scroll.region.start,
                          dmg->scroll.lines) * cols,
        },
    };
    grid_render_erase(c, buf, &erase);
}

static void
grid_render_scroll_reverse(struct context *c, struct buffer *buf,
                           const struct damage *dmg)
{
    int src_y = (dmg->scroll.region.start + 0) * c->term.cell_height;
    int dst_y = (dmg->scroll.region.start + dmg->scroll.lines) * c->term.cell_height;
    int width = buf->width;
    int height = (dmg->scroll.region.end - dmg->scroll.region.start - dmg->scroll.lines) * c->term.cell_height;

    const uint32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

    LOG_DBG("damage: SCROLL REVERSE: %d-%d by %d lines (dst-y: %d, src-y: %d, "
            "height: %d, stride: %d, mmap-size: %zu)",
            dmg->scroll.region.start, dmg->scroll.region.end,
            dmg->scroll.lines,
            dst_y, src_y, height, stride,
            buf->size);

    if (height > 0) {
        cairo_surface_flush(buf->cairo_surface);
        uint8_t *raw = cairo_image_surface_get_data(buf->cairo_surface);

        memmove(raw + dst_y * stride, raw + src_y * stride, height * stride);
        cairo_surface_mark_dirty(buf->cairo_surface);

        wl_surface_damage_buffer(c->wl.surface, 0, dst_y, width, height);
    }

    const int cols = c->term.cols;

    struct damage erase = {
        .type = DAMAGE_ERASE,
        .range = {
            .start = dmg->scroll.region.start * cols,
            .length = min(dmg->scroll.region.end - dmg->scroll.region.start,
                          dmg->scroll.lines) * cols,
        },
    };
    grid_render_erase(c, buf, &erase);
}

static void
grid_render(struct context *c)
{
    struct buffer *buf = shm_get_buffer(c->wl.shm, c->width, c->height);
    cairo_set_operator(buf->cairo, CAIRO_OPERATOR_SOURCE);

    if (tll_length(c->term.grid->damage) == 0 &&
        tll_length(c->term.grid->scroll_damage) == 0)
    {
        goto render_cursor;
    }

    assert(c->width > 0);
    assert(c->height > 0);



    tll_foreach(c->term.grid->scroll_damage, it) {
        switch (it->item.type) {
        case DAMAGE_SCROLL:
            grid_render_scroll(c, buf, &it->item);
            break;

        case DAMAGE_SCROLL_REVERSE:
            grid_render_scroll_reverse(c, buf, &it->item);
            break;

        case DAMAGE_UPDATE:
        case DAMAGE_ERASE:
            assert(false);
            break;
        }

        tll_remove(c->term.grid->scroll_damage, it);
    }

    tll_foreach(c->term.grid->damage, it) {
        switch (it->item.type) {
        case DAMAGE_ERASE:  grid_render_erase(c, buf, &it->item); break;
        case DAMAGE_UPDATE: grid_render_update(c, buf, &it->item); break;

        case DAMAGE_SCROLL:
        case DAMAGE_SCROLL_REVERSE:
            assert(false);
            break;
        }

        tll_remove(c->term.grid->damage, it);
    }

render_cursor:
    ;

    /* TODO: break out to function */
    /* Re-render last cursor cell and current cursor cell */
    static struct cursor last_cursor = {.linear = 0, .col = 0, .row = 0};

    if (last_cursor.linear != c->term.cursor.linear) {
        struct damage prev_cursor = {
            .type = DAMAGE_UPDATE,
            .range = {.start = c->term.grid->offset + last_cursor.linear,
                      .length = 1},
        };
        grid_render_update(c, buf, &prev_cursor);
    }

    struct damage cursor = {
        .type = DAMAGE_UPDATE,
        .range = {.start = c->term.grid->offset + c->term.cursor.linear, .length = 1},
    };
    grid_render_update(c, buf, &cursor);
    last_cursor = c->term.cursor;

    c->term.grid->offset %= c->term.grid->size;

    //cairo_surface_flush(buf->cairo_surface);
    wl_surface_attach(c->wl.surface, buf->wl_buf, 0, 0);

    struct wl_callback *cb = wl_surface_frame(c->wl.surface);
    wl_callback_add_listener(cb, &frame_listener, c);
    c->frame_is_scheduled = true;

    wl_surface_commit(c->wl.surface);
}

static void
frame_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct context *c = data;

    c->frame_is_scheduled = false;
    wl_callback_destroy(wl_callback);
    grid_render(c);
}

static void
resize(struct context *c, int width, int height)
{
    if (width == c->width && height == c->height)
        return;

    c->width = width;
    c->height = height;

    const size_t old_rows = c->term.rows;
    const size_t normal_old_size = c->term.normal.size;
    const size_t alt_old_size = c->term.alt.size;

    c->term.cols = c->width / c->term.cell_width;
    c->term.rows = c->height / c->term.cell_height;

    c->term.normal.size = c->term.cols * c->term.rows;
    c->term.alt.size = c->term.cols * c->term.rows;

    c->term.normal.cells = realloc(
        c->term.normal.cells,
        c->term.normal.size * sizeof(c->term.normal.cells[0]));
    c->term.alt.cells = realloc(
        c->term.alt.cells,
        c->term.alt.size * sizeof(c->term.alt.cells[0]));

    for (size_t i = normal_old_size; i < c->term.normal.size; i++) {
        c->term.normal.cells[i] = (struct cell){
            .attrs = {.foreground = default_foreground,
                      .background = default_background},
        };
    }

    for (size_t i = alt_old_size; i < c->term.alt.size; i++) {
        c->term.alt.cells[i] = (struct cell){
            .attrs = {.foreground = default_foreground,
                      .background = default_background},
        };
    }

    LOG_INFO("resize: %dx%d, grid: cols=%d, rows=%d",
             c->width, c->height, c->term.cols, c->term.rows);

    /* Update environment variables */
    char cols_s[12], rows_s[12];
    sprintf(cols_s, "%d", c->term.cols);
    sprintf(rows_s, "%d", c->term.rows);
    setenv("COLUMNS", cols_s, 1);
    setenv("LINES", rows_s, 1);

    /* Signal TIOCSWINSZ */
    if (ioctl(c->term.ptmx, TIOCSWINSZ,
              &(struct winsize){
                  .ws_row = c->term.rows,
                  .ws_col = c->term.cols,
                  .ws_xpixel = c->width,
                  .ws_ypixel = c->height}) == -1)
    {
        LOG_ERRNO("TIOCSWINSZ");
    }

    if (c->term.scroll_region.end == old_rows)
        c->term.scroll_region.end = c->term.rows;

    term_damage_all(&c->term);

    if (!c->frame_is_scheduled)
        grid_render(c);
}

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
}

static const struct wl_shm_listener shm_listener = {
    .format = &shm_format,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    LOG_DBG("wm base ping");
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = &xdg_wm_base_ping,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
                         enum wl_seat_capability caps)
{
    struct context *c = data;

    if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD))
        return;

    if (c->wl.keyboard != NULL)
        wl_keyboard_release(c->wl.keyboard);

    c->wl.keyboard = wl_seat_get_keyboard(wl_seat);
    wl_keyboard_add_listener(c->wl.keyboard, &keyboard_listener, &c->term);
}

static void
seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

static void
handle_global(void *data, struct wl_registry *registry,
              uint32_t name, const char *interface, uint32_t version)
{
    //LOG_DBG("global: %s", interface);
    struct context *c = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        c->wl.compositor = wl_registry_bind(
            c->wl.registry, name, &wl_compositor_interface, 4);
    }

    else if (strcmp(interface, wl_shm_interface.name) == 0) {
        c->wl.shm = wl_registry_bind(c->wl.registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(c->wl.shm, &shm_listener, &c->wl);
        wl_display_roundtrip(c->wl.display);
    }

    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        c->wl.shell = wl_registry_bind(
            c->wl.registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(c->wl.shell, &xdg_wm_base_listener, c);
    }

    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        c->wl.seat = wl_registry_bind(c->wl.registry, name, &wl_seat_interface, 4);
        wl_seat_add_listener(c->wl.seat, &seat_listener, c);
        wl_display_roundtrip(c->wl.display);
    }

#if 0
    else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *output = wl_registry_bind(
            c->wl.registry, name, &wl_output_interface, 3);

        tll_push_back(c->wl.monitors, ((struct monitor){.output = output}));

        struct monitor *mon = &tll_back(c->wl.monitors);
        wl_output_add_listener(output, &output_listener, mon);

        /*
         * The "output" interface doesn't give us the monitors'
         * identifiers (e.g. "LVDS-1"). Use the XDG output interface
         * for that.
         */

        assert(c->wl.xdg_output_manager != NULL);
        mon->xdg = zxdg_output_manager_v1_get_xdg_output(
            c->wl.xdg_output_manager, mon->output);

        zxdg_output_v1_add_listener(mon->xdg, &xdg_output_listener, mon);
        wl_display_roundtrip(c->wl.display);
    }
#endif

#if 0
    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        c->wl.layer_shell = wl_registry_bind(
            c->wl.registry, name, &zwlr_layer_shell_v1_interface, 1);
    }
#endif

#if 0
    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        c->wl.seat = wl_registry_bind(c->wl.registry, name, &wl_seat_interface, 4);
        wl_seat_add_listener(c->wl.seat, &seat_listener, c);
        wl_display_roundtrip(c->wl.display);
    }
#endif

#if 0
    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        c->wl.xdg_output_manager = wl_registry_bind(
            c->wl.registry, name, &zxdg_output_manager_v1_interface, 2);
    }
#endif
}

static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                       int32_t width, int32_t height, struct wl_array *states)
{
    //struct context *c = data;
    //LOG_DBG("xdg-toplevel: configure: %dx%d", width, height);
    if (width <= 0 || height <= 0)
        return;

    resize(data, width, height);
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    struct context *c = data;
    LOG_DBG("xdg-toplevel: close");
    c->quit = true;
    wl_display_roundtrip(c->wl.display);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = &xdg_toplevel_configure,
    .close = &xdg_toplevel_close,
};

static void
xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                      uint32_t serial)
{
    //LOG_DBG("xdg-surface: configure");
    xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = &xdg_surface_configure,
};

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    LOG_WARN("global removed: %u", name);
    assert(false);
}

static const struct wl_registry_listener registry_listener = {
    .global = &handle_global,
    .global_remove = &handle_global_remove,
};

static int
keyboard_repeater(void *arg)
{
    struct terminal *term = arg;

    while (true) {
        LOG_DBG("repeater: waiting for start");

        mtx_lock(&term->kbd.repeat.mutex);
        while (term->kbd.repeat.cmd == REPEAT_STOP)
            cnd_wait(&term->kbd.repeat.cond, &term->kbd.repeat.mutex);

        if (term->kbd.repeat.cmd == REPEAT_EXIT) {
            mtx_unlock(&term->kbd.repeat.mutex);
            return 0;
        }

    restart:

        LOG_DBG("repeater: started");
        assert(term->kbd.repeat.cmd == REPEAT_START);

        const long rate_delay = 1000000000 / term->kbd.repeat.rate;
        long delay = term->kbd.repeat.delay * 1000000;

        while (true) {
            assert(term->kbd.repeat.rate > 0);

            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);

            timeout.tv_nsec += delay;
            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec += timeout.tv_nsec / 1000000000;
                timeout.tv_nsec %= 1000000000;
            }

            int ret = cnd_timedwait(&term->kbd.repeat.cond, &term->kbd.repeat.mutex, &timeout);
            if (ret == thrd_success) {
                if (term->kbd.repeat.cmd == REPEAT_START)
                    goto restart;
                else if (term->kbd.repeat.cmd == REPEAT_STOP) {
                    mtx_unlock(&term->kbd.repeat.mutex);
                    break;
                } else if (term->kbd.repeat.cmd == REPEAT_EXIT) {
                    mtx_unlock(&term->kbd.repeat.mutex);
                    return 0;
                }
            }

            assert(ret == thrd_timedout);
            assert(term->kbd.repeat.cmd == REPEAT_START);
            LOG_DBG("repeater: repeat: %u", term->kbd.repeat.key);

            if (write(term->kbd.repeat.pipe_write_fd, &term->kbd.repeat.key,
                      sizeof(term->kbd.repeat.key)) != sizeof(term->kbd.repeat.key))
            {
                LOG_ERRNO("faile to write repeat key to repeat pipe");
                mtx_unlock(&term->kbd.repeat.mutex);
                return 0;
            }

            delay = rate_delay;
        }

    }

    assert(false);
    return 1;
}

int
main(int argc, const char *const *argv)
{
    int ret = EXIT_FAILURE;

    setlocale(LC_ALL, "");

    int repeat_pipe_fds[2] = {-1, -1};
    if (pipe2(repeat_pipe_fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe for repeater thread");
        return ret;
    }

    struct context c = {
        .quit = false,
        .term = {
            .ptmx = posix_openpt(O_RDWR | O_NOCTTY),
            .decckm = DECCKM_CSI,
            .keypad_mode = KEYPAD_NUMERICAL,  /* TODO: verify */
            .vt = {
                .state = 1,  /* STATE_GROUND */
            },
            .kbd = {
                .repeat = {
                    .pipe_read_fd = repeat_pipe_fds[0],
                    .pipe_write_fd = repeat_pipe_fds[1],
                    .cmd = REPEAT_STOP,
                },
            },
            .foreground = default_foreground,
            .background = default_background,

            .normal = {.damage = tll_init(), .scroll_damage = tll_init()},
            .alt = {.damage = tll_init(), .scroll_damage = tll_init()},
            .grid = &c.term.normal,
        },
    };

    mtx_init(&c.term.kbd.repeat.mutex, mtx_plain);
    cnd_init(&c.term.kbd.repeat.cond);

    thrd_t keyboard_repeater_id;
    thrd_create(&keyboard_repeater_id, &keyboard_repeater, &c.term);

    const char *font_name = "Dina:pixelsize=12";
    c.fonts[0].font = font_from_name(font_name);
    if (c.fonts[0].font == NULL)
        goto out;

    {
        char fname[1024];
        snprintf(fname, sizeof(fname), "%s:style=bold", font_name);
        c.fonts[1].font = font_from_name(fname);

        snprintf(fname, sizeof(fname), "%s:style=italic", font_name);
        c.fonts[2].font = font_from_name(fname);

        snprintf(fname, sizeof(fname), "%s:style=bold italic", font_name);
        c.fonts[3].font = font_from_name(fname);
    }

    cairo_scaled_font_extents(c.fonts[0].font,  &c.fextents);
    c.term.cell_width = (int)ceil(c.fextents.max_x_advance);
    c.term.cell_height = (int)ceil(c.fextents.height);

    LOG_DBG("font: height: %.2f, x-advance: %.2f",
            c.fextents.height, c.fextents.max_x_advance);
    assert(c.fextents.max_y_advance == 0);

    for (size_t i; i < sizeof(c.fonts) / sizeof(c.fonts[0]); i++) {
        cairo_scaled_font_t *font = c.fonts[i].font;

        for (size_t j = 0; j < 256; j++) {
            const char text[2] = {(char)j, '\0'};

            cairo_glyph_t *glyphs = NULL;
            int num_glyphs = 0;

            cairo_status_t status = cairo_scaled_font_text_to_glyphs(
                font, 0, 0 + c.fextents.ascent,
                text, 1, &glyphs, &num_glyphs, NULL, NULL, NULL);

            if (status != CAIRO_STATUS_SUCCESS || num_glyphs != 1) {
                if (glyphs != NULL)
                    cairo_glyph_free(glyphs);
                continue;
            }

            cairo_surface_t *surf = cairo_image_surface_create(
                CAIRO_FORMAT_ARGB32, c.term.cell_width, c.term.cell_height);
            cairo_t *ca = cairo_create(surf);

            cairo_set_scaled_font(ca, font);
            cairo_set_source_rgba(ca, 1.0, 1.0, 1.0, 1.0);
            cairo_show_glyphs(ca, glyphs, num_glyphs);

            cairo_glyph_free(glyphs);
            cairo_destroy(ca);

            assert(surf != NULL);
            assert(cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS);
            c.fonts[i].surf[j] = surf;

            cairo_pattern_t *pat = cairo_pattern_create_for_surface(surf);
            assert(cairo_pattern_status(pat) == CAIRO_STATUS_SUCCESS);
            c.fonts[i].cache[j] = pat;
        }
    }

    if (c.term.ptmx == -1) {
        LOG_ERRNO("failed to open pseudo terminal");
        goto out;
    }

    c.wl.display = wl_display_connect(NULL);
    if (c.wl.display == NULL) {
        LOG_ERR("failed to connect to wayland; no compositor running?");
        goto out;
    }

    c.wl.registry = wl_display_get_registry(c.wl.display);
    if (c.wl.registry == NULL) {
        LOG_ERR("failed to get wayland registry");
        goto out;
    }

    wl_registry_add_listener(c.wl.registry, &registry_listener, &c);
    wl_display_roundtrip(c.wl.display);

    if (c.wl.compositor == NULL) {
        LOG_ERR("no compositor");
        goto out;
    }
    if (c.wl.shm == NULL) {
        LOG_ERR("no shared memory buffers interface");
        goto out;
    }
    if (c.wl.shell == NULL) {
        LOG_ERR("no XDG shell interface");
        goto out;
    }

    c.wl.surface = wl_compositor_create_surface(c.wl.compositor);
    if (c.wl.surface == NULL) {
        LOG_ERR("failed to create wayland surface");
        goto out;
    }

    c.wl.xdg_surface = xdg_wm_base_get_xdg_surface(c.wl.shell, c.wl.surface);
    xdg_surface_add_listener(c.wl.xdg_surface, &xdg_surface_listener, &c);

    c.wl.xdg_toplevel = xdg_surface_get_toplevel(c.wl.xdg_surface);
    xdg_toplevel_add_listener(c.wl.xdg_toplevel, &xdg_toplevel_listener, &c);

    xdg_toplevel_set_app_id(c.wl.xdg_toplevel, "f00ter");
    xdg_toplevel_set_title(c.wl.xdg_toplevel, "f00ter");

    wl_surface_commit(c.wl.surface);
    wl_display_roundtrip(c.wl.display);

    /* TODO: use font metrics to calculate initial size from ROWS x COLS */
    const int default_width = 300;
    const int default_height = 300;
    resize(&c, default_width, default_height);

    wl_display_dispatch_pending(c.wl.display);

    c.term.slave = fork();
    switch (c.term.slave) {
    case -1:
        LOG_ERRNO("failed to fork");
        goto out;

    case 0:
        /* Child */
        slave_spawn(c.term.ptmx);
        assert(false);
        break;

    default:
        LOG_DBG("slave has PID %d", c.term.slave);
        break;
    }

    while (true) {
        struct pollfd fds[] = {
            {.fd = wl_display_get_fd(c.wl.display), .events = POLLIN},
            {.fd = c.term.ptmx,                     .events = POLLIN},
            {.fd = c.term.kbd.repeat.pipe_read_fd,  .events = POLLIN},
        };

        wl_display_flush(c.wl.display);
        poll(fds, sizeof(fds) / sizeof(fds[0]), -1);

        if (fds[0].revents & POLLIN) {
            wl_display_dispatch(c.wl.display);
            if (c.quit) {
                ret = EXIT_SUCCESS;
                break;
            }
        }

        if (fds[0].revents & POLLHUP) {
            LOG_WARN("disconnected from wayland");
            break;
        }

        if (fds[1].revents & POLLIN) {
            uint8_t data[8192];
            ssize_t count = read(c.term.ptmx, data, sizeof(data));
            if (count < 0) {
                LOG_ERRNO("failed to read from pseudo terminal");
                break;
            }

            //LOG_DBG("%.*s", (int)count, data);

            vt_from_slave(&c.term, data, count);
            if (!c.frame_is_scheduled)
                grid_render(&c);
        }

        if (fds[1].revents & POLLHUP) {
            ret = EXIT_SUCCESS;
            break;
        }

        if (fds[2].revents & POLLIN) {
            uint32_t key;
            if (read(c.term.kbd.repeat.pipe_read_fd, &key, sizeof(key)) != sizeof(key)) {
                LOG_ERRNO("failed to read repeat key from repeat pipe");
                break;
            }

            c.term.kbd.repeat.dont_re_repeat = true;
            input_repeat(&c.term, key);
            c.term.kbd.repeat.dont_re_repeat = false;
        }

        if (fds[2].revents & POLLHUP)
            LOG_ERR("keyboard repeat handling thread died");
    }

out:
    mtx_lock(&c.term.kbd.repeat.mutex);
    c.term.kbd.repeat.cmd = REPEAT_EXIT;
    cnd_signal(&c.term.kbd.repeat.cond);
    mtx_unlock(&c.term.kbd.repeat.mutex);

    shm_fini();
    if (c.wl.xdg_toplevel != NULL)
        xdg_toplevel_destroy(c.wl.xdg_toplevel);
    if (c.wl.xdg_surface != NULL)
        xdg_surface_destroy(c.wl.xdg_surface);
    if (c.wl.surface != NULL)
        wl_surface_destroy(c.wl.surface);
    if (c.wl.shell != NULL)
        xdg_wm_base_destroy(c.wl.shell);
    if (c.wl.shm != NULL)
        wl_shm_destroy(c.wl.shm);
    if (c.wl.compositor != NULL)
        wl_compositor_destroy(c.wl.compositor);
    if (c.wl.registry != NULL)
        wl_registry_destroy(c.wl.registry);
    if (c.wl.display != NULL)
        wl_display_disconnect(c.wl.display);

    free(c.term.normal.cells);
    free(c.term.alt.cells);

    for (size_t i = 0; i < sizeof(c.fonts) / sizeof(c.fonts[0]); i++) {
        struct glyph_cache *fcache = &c.fonts[i];
        if (fcache->font != NULL)
            cairo_scaled_font_destroy(fcache->font);

        for (size_t i = 0; i < sizeof(fcache->cache) / sizeof(fcache->cache[0]); i++) {
            if (fcache->cache[i] != NULL)
                cairo_pattern_destroy(fcache->cache[i]);
        }
    }

    if (c.term.ptmx != -1)
        close(c.term.ptmx);

    thrd_join(keyboard_repeater_id, NULL);
    cnd_destroy(&c.term.kbd.repeat.cond);
    mtx_destroy(&c.term.kbd.repeat.mutex);
    close(c.term.kbd.repeat.pipe_read_fd);
    close(c.term.kbd.repeat.pipe_write_fd);

    cairo_debug_reset_static_data();
    return ret;
}
