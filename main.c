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

static const uint32_t default_foreground = 0xffffffff;
static const uint32_t default_background = 0x000000ff;

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

struct context {
    bool quit;

    cairo_scaled_font_t *fonts[8];
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

static cairo_scaled_font_t *
attrs_to_font(struct context *c, const struct attributes *attrs)
{
    int idx = attrs->italic << 1 | attrs->bold;
    return c->fonts[idx];
}

static void
grid_render_update(struct context *c, struct buffer *buf, const struct damage *dmg)
{
    LOG_DBG("damage: UPDATE: %d -> %d",
            dmg->range.start, dmg->range.start + dmg->range.length);

    const int cols = c->term.grid.cols;

    for (int linear_cursor = dmg->range.start,
             row = dmg->range.start / cols,
             col = dmg->range.start % cols;
         linear_cursor < dmg->range.start + dmg->range.length;
         linear_cursor++,
             col = (col + 1) % cols,
             row += col == 0 ? 1 : 0)
    {
        //LOG_DBG("UPDATE: %d (%dx%d)", linear_cursor, row, col);

        const struct cell *cell = &c->term.grid.cells[linear_cursor];
        bool has_cursor = c->term.grid.linear_cursor == linear_cursor;

        int x = col * c->term.grid.cell_width;
        int y = row * c->term.grid.cell_height;
        int width = c->term.grid.cell_width;
        int height = c->term.grid.cell_height;

        uint32_t foreground = cell->attrs.foreground;
        uint32_t background = cell->attrs.background;

        if (has_cursor) {
            uint32_t swap = foreground;
            foreground = background;
            background = swap;
        }

        if (cell->attrs.reverse) {
            uint32_t swap = foreground;
            foreground = background;
            background = swap;
        }

        //LOG_DBG("cell %dx%d dirty: c=0x%02x (%c)",
        //        row, col, cell->c[0], cell->c[0]);

        double br = (double)((background >> 24) & 0xff) / 255.0;
        double bg = (double)((background >> 16) & 0xff) / 255.0;
        double bb = (double)((background >>  8) & 0xff) / 255.0;

        double fr = (double)((foreground >> 24) & 0xff) / 255.0;
        double fg = (double)((foreground >> 16) & 0xff) / 255.0;
        double fb = (double)((foreground >>  8) & 0xff) / 255.0;

        cairo_scaled_font_t *font = attrs_to_font(c, &cell->attrs);
        cairo_set_scaled_font(buf->cairo, font);
        cairo_set_source_rgba(buf->cairo, br, bg, bb, 1.0);

        /* Background */
        cairo_rectangle(buf->cairo, x, y, width, height);
        cairo_fill(buf->cairo);

        if (cell->attrs.conceal)
            continue;

        cairo_glyph_t *glyphs = NULL;
        int num_glyphs = 0;

        cairo_status_t status = cairo_scaled_font_text_to_glyphs(
            font, x, y + c->fextents.ascent,
            cell->c, strlen(cell->c), &glyphs, &num_glyphs,
            NULL, NULL, NULL);

        if (status != CAIRO_STATUS_SUCCESS) {
            if (glyphs != NULL)
                cairo_glyph_free(glyphs);
            continue;
        }

        cairo_set_source_rgba(buf->cairo, fr, fg, fb, 1.0);
        cairo_show_glyphs(buf->cairo, glyphs, num_glyphs);
        cairo_glyph_free(glyphs);
    }

    wl_surface_damage_buffer(
        c->wl.surface,
        0, (dmg->range.start / cols) * c->term.grid.cell_height,
        buf->width, (dmg->range.length + cols - 1) / cols * c->term.grid.cell_height);
}

static void
grid_render_erase(struct context *c, struct buffer *buf, const struct damage *dmg)
{
    LOG_DBG("damage: ERASE: %d -> %d",
            dmg->range.start, dmg->range.start + dmg->range.length);

    double br = (double)((default_background >> 24) & 0xff) / 255.0;
    double bg = (double)((default_background >> 16) & 0xff) / 255.0;
    double bb = (double)((default_background >>  8) & 0xff) / 255.0;
    cairo_set_source_rgba(buf->cairo, br, bg, bb, 1.0);

    const int cols = c->term.grid.cols;

    int start = dmg->range.start;
    int left = dmg->range.length;

    int row = start / cols;
    int col = start % cols;

    /* Partial initial line */
    if (col != 0) {
        int cell_count = min(left, cols - col);

        int x = col * c->term.grid.cell_width;
        int y = row * c->term.grid.cell_height;
        int width = cell_count * c->term.grid.cell_width;
        int height = c->term.grid.cell_height;

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
        int y = row * c->term.grid.cell_height;
        int width = buf->width;
        int height = line_count * c->term.grid.cell_height;

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
        int y = row * c->term.grid.cell_height;
        int width = left * c->term.grid.cell_width;
        int height = c->term.grid.cell_height;

        cairo_rectangle(buf->cairo, x, y, width, height);
        cairo_fill(buf->cairo);
        wl_surface_damage_buffer(c->wl.surface, x, y, width, height);
    }

    /* Redraw cursor, if it's inside the erased range */
    if (c->term.grid.linear_cursor >= dmg->range.start &&
        c->term.grid.linear_cursor < dmg->range.start + dmg->range.length)
    {
        grid_render_update(
            c, buf,
            &(struct damage){
                .type = DAMAGE_UPDATE,
                .range = {.start = c->term.grid.linear_cursor, .length = 1}});
    }
}

static void
grid_render_scroll(struct context *c, struct buffer *buf,
                   const struct damage *dmg)
{
    //int x = 0;
    int dst_y = (dmg->scroll.top_margin + 0) * c->term.grid.cell_height;
    int src_y = (dmg->scroll.top_margin + dmg->scroll.lines) * c->term.grid.cell_height;
    int width = buf->width;
    int height = (c->term.grid.rows -
                  dmg->scroll.top_margin -
                  dmg->scroll.bottom_margin -
                  dmg->scroll.lines) * c->term.grid.cell_height;

    const uint32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

    LOG_DBG("damage: SCROLL: %d-%d by %d lines (dst-y: %d, src-y: %d, height: %d, stride: %d, mmap-size: %zu)",
            dmg->scroll.top_margin,
            c->term.grid.rows - dmg->scroll.bottom_margin,
            dmg->scroll.lines,
            dst_y, src_y, height, stride,
            buf->size);

    cairo_surface_flush(buf->cairo_surface);
    uint8_t *raw = cairo_image_surface_get_data(buf->cairo_surface);

    memmove(raw + dst_y * stride, raw + src_y * stride, height * stride);
    cairo_surface_mark_dirty(buf->cairo_surface);

    wl_surface_damage_buffer(c->wl.surface, 0, dst_y, width, height);

#if 1
    const int cols = c->term.grid.cols;
    struct damage erase = {
        .type = DAMAGE_ERASE,
        .range = {
            .start = (c->term.grid.rows -
                      dmg->scroll.bottom_margin -
                      dmg->scroll.lines) * cols,
            .length = dmg->scroll.lines * cols
        },
    };
    grid_render_erase(c, buf, &erase);
#endif
}

static void
grid_render(struct context *c)
{
    if (tll_length(c->term.grid.damage) == 0)
        return;

    assert(c->width > 0);
    assert(c->height > 0);

    struct buffer *buf = shm_get_buffer(c->wl.shm, c->width, c->height);

    cairo_set_operator(buf->cairo, CAIRO_OPERATOR_SOURCE);

    //bool scroll = false;
    tll_foreach(c->term.grid.damage, it) {
        switch (it->item.type) {
        case DAMAGE_ERASE:
            grid_render_erase(c, buf, &it->item);
            break;

        case DAMAGE_UPDATE:
            grid_render_update(c, buf, &it->item);
            break;

        case DAMAGE_SCROLL:
            //scroll = true;
            grid_render_scroll(c, buf, &it->item);
            break;
        }

        tll_remove(c->term.grid.damage, it);
    }

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

    bool alt_screen_active = c->term.grid.cells == c->term.grid.alt_grid;

    c->width = width;
    c->height = height;

    size_t old_cells_len = c->term.grid.cols * c->term.grid.rows;

    c->term.grid.cell_width = (int)ceil(c->fextents.max_x_advance);
    c->term.grid.cell_height = (int)ceil(c->fextents.height);
    c->term.grid.cols = c->width / c->term.grid.cell_width;
    c->term.grid.rows = c->height / c->term.grid.cell_height;

    c->term.grid.normal_grid = realloc(
        c->term.grid.normal_grid,
        c->term.grid.cols * c->term.grid.rows * sizeof(c->term.grid.cells[0]));
    c->term.grid.alt_grid = realloc(
        c->term.grid.alt_grid,
        c->term.grid.cols * c->term.grid.rows * sizeof(c->term.grid.cells[0]));

    size_t new_cells_len = c->term.grid.cols * c->term.grid.rows;
    for (size_t i = old_cells_len; i < new_cells_len; i++) {
        c->term.grid.normal_grid[i] = (struct cell){
            .attrs = {.foreground = default_foreground,
                      .background = default_background},
        };
        c->term.grid.alt_grid[i] = (struct cell){
            .attrs = {.foreground = default_foreground,
                      .background = default_background},
        };
    }

    c->term.grid.cells = alt_screen_active
        ? c->term.grid.alt_grid : c->term.grid.normal_grid;

    LOG_DBG("resize: %dx%d, grid: cols=%d, rows=%d",
            c->width, c->height, c->term.grid.cols, c->term.grid.rows);

    /* Update environment variables */
    char cols_s[12], rows_s[12];
    sprintf(cols_s, "%u", c->term.grid.cols);
    sprintf(rows_s, "%u", c->term.grid.rows);
    setenv("COLUMNS", cols_s, 1);
    setenv("LINES", rows_s, 1);

    /* SIignal TIOCSWINSZ */
    if (ioctl(c->term.ptmx, TIOCSWINSZ,
              &(struct winsize){
                  .ws_row = c->term.grid.rows,
                      .ws_col = c->term.grid.cols,
                      .ws_xpixel = c->width,
                      .ws_ypixel = c->height}) == -1)
    {
        LOG_ERRNO("TIOCSWINSZ");
    }

    tll_free(c->term.grid.damage);
    assert(tll_length(c->term.grid.damage) == 0);
    grid_damage_update(&c->term.grid, 0, c->term.grid.rows * c->term.grid.cols);

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
            .grid = {.foreground = default_foreground,
                     .background = default_background,
                     .damage = tll_init()},
            .kbd = {
                .repeat = {
                    .pipe_read_fd = repeat_pipe_fds[0],
                    .pipe_write_fd = repeat_pipe_fds[1],
                    .cmd = REPEAT_STOP,
                },
            },
        },
    };

    mtx_init(&c.term.kbd.repeat.mutex, mtx_plain);
    cnd_init(&c.term.kbd.repeat.cond);

    thrd_t keyboard_repeater_id;
    thrd_create(&keyboard_repeater_id, &keyboard_repeater, &c.term);

    const char *font_name = "Dina:pixelsize=12";
    c.fonts[0] = font_from_name(font_name);
    if (c.fonts[0] == NULL)
        goto out;

    {
        char fname[1024];
        snprintf(fname, sizeof(fname), "%s:style=bold", font_name);
        c.fonts[1] = font_from_name(fname);

        snprintf(fname, sizeof(fname), "%s:style=italic", font_name);
        c.fonts[2] = font_from_name(fname);

        snprintf(fname, sizeof(fname), "%s:style=bold italic", font_name);
        c.fonts[3] = font_from_name(fname);

        /* TODO; underline */
    }

    cairo_scaled_font_extents(c.fonts[0],  &c.fextents);

    LOG_DBG("font: height: %.2f, x-advance: %.2f",
            c.fextents.height, c.fextents.max_x_advance);
    assert(c.fextents.max_y_advance == 0);

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
            {.fd = c.term.ptmx, .events = POLLIN},
            {.fd = c.term.kbd.repeat.pipe_read_fd, .events = POLLIN},
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
            uint8_t data[1024];
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

    free(c.term.grid.normal_grid);
    free(c.term.grid.alt_grid);

    for (size_t i = 0; i < sizeof(c.fonts) / sizeof(c.fonts[0]); i++) {
        if (c.fonts[i] != NULL)
            cairo_scaled_font_destroy(c.fonts[i]);
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
