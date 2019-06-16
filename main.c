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

static const uint32_t default_foreground = 0xffffffff;
static const uint32_t default_background = 0x000000ff;

struct wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    struct wl_shm *shm;
    struct xdg_wm_base *shell;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
};

struct context {
    bool quit;
    int ptmx;

    cairo_scaled_font_t *font;
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

static void
grid_render(struct context *c)
{
    assert(c->term.grid.dirty);
    assert(c->width > 0);
    assert(c->height > 0);

    struct buffer *buf = shm_get_buffer(c->wl.shm, c->width, c->height);

    double br, bg, bb;  /* Background */
    double fr, fg, fb;  /* Foreground */

    cairo_set_operator(buf->cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_scaled_font(buf->cairo, c->font);

    if (c->term.grid.all_dirty) {
        br = (double)((default_background >> 24) & 0xff) / 255.0;
        bg = (double)((default_background >> 16) & 0xff) / 255.0;
        bb = (double)((default_background >>  8) & 0xff) / 255.0;
        cairo_set_source_rgba(buf->cairo, br, bg, bb, 1.0);
        cairo_rectangle(buf->cairo, 0, 0, buf->width, buf->height);
        cairo_fill(buf->cairo);
    }


    for (int row = 0; row < c->term.grid.rows; row++) {
        for (int col = 0; col < c->term.grid.cols; col++) {
            int cell_idx = row * c->term.grid.cols + col;
            struct cell *cell = &c->term.grid.cells[cell_idx];

            if (!cell->dirty && !c->term.grid.all_dirty)
                continue;

            cell->dirty = false;

            bool has_cursor = c->term.grid.cursor == cell_idx;

            int y_ofs = row * c->term.grid.cell_height + c->fextents.ascent;
            int x_ofs = col * c->term.grid.cell_width;

            int damage_x = x_ofs;
            int damage_y = y_ofs - c->fextents.ascent;

            //LOG_DBG("cell %dx%d dirty: c=0x%02x (%c)",
            //        row, col, cell->c[0], cell->c[0]);

            br = (double)((cell->attrs.background >> 24) & 0xff) / 255.0;
            bg = (double)((cell->attrs.background >> 16) & 0xff) / 255.0;
            bb = (double)((cell->attrs.background >>  8) & 0xff) / 255.0;

            fr = (double)((cell->attrs.foreground >> 24) & 0xff) / 255.0;
            fg = (double)((cell->attrs.foreground >> 16) & 0xff) / 255.0;
            fb = (double)((cell->attrs.foreground >>  8) & 0xff) / 255.0;

            if (has_cursor)
                cairo_set_source_rgba(buf->cairo, fr, fg, fb, 1.0);
            else
                cairo_set_source_rgba(buf->cairo, br, bg, bb, 1.0);

            cairo_rectangle(
                buf->cairo, damage_x, damage_y,
                c->term.grid.cell_width, c->term.grid.cell_height);
            cairo_fill(buf->cairo);

            cairo_glyph_t *glyphs = NULL;
            int num_glyphs = 0;

            cairo_status_t status = cairo_scaled_font_text_to_glyphs(
                c->font, x_ofs, y_ofs, cell->c, strlen(cell->c),
                &glyphs, &num_glyphs, NULL, NULL, NULL);

            //assert(status == CAIRO_STATUS_SUCCESS);
            if (status != CAIRO_STATUS_SUCCESS) {
                if (glyphs != NULL)
                    cairo_glyph_free(glyphs);
                continue;
            }

            if (has_cursor)
                cairo_set_source_rgba(buf->cairo, br, bg, bb, 1.0);
            else
                cairo_set_source_rgba(buf->cairo, fr, fg, fb, 1.0);
            cairo_show_glyphs(buf->cairo, glyphs, num_glyphs);
            cairo_glyph_free(glyphs);

            wl_surface_damage_buffer(
                c->wl.surface, damage_x, damage_y,
                c->term.grid.cell_width, c->term.grid.cell_height);
        }
    }

    wl_surface_attach(c->wl.surface, buf->wl_buf, 0, 0);

    struct wl_callback *cb = wl_surface_frame(c->wl.surface);
    wl_callback_add_listener(cb, &frame_listener, c);
    c->frame_is_scheduled = true;

    wl_surface_commit(c->wl.surface);
    c->term.grid.dirty = c->term.grid.all_dirty = false;
}

static void
frame_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct context *c = data;

    c->frame_is_scheduled = false;
    wl_callback_destroy(wl_callback);

    if (c->term.grid.dirty)
        grid_render(c);
}

static void
resize(struct context *c, int width, int height)
{
    if (width == c->width && height == c->height)
        return;

    c->width = width;
    c->height = height;

    size_t old_cells_len = c->term.grid.cols * c->term.grid.rows;

    c->term.grid.cell_width = (int)ceil(c->fextents.max_x_advance);
    c->term.grid.cell_height = (int)ceil(c->fextents.height);
    c->term.grid.cols = c->width / c->term.grid.cell_width;
    c->term.grid.rows = c->height / c->term.grid.cell_height;
    c->term.grid.cells = realloc(c->term.grid.cells,
        c->term.grid.cols * c->term.grid.rows * sizeof(c->term.grid.cells[0]));

    size_t new_cells_len = c->term.grid.cols * c->term.grid.rows;
    for (size_t i = old_cells_len; i < new_cells_len; i++) {
        c->term.grid.cells[i] = (struct cell){
            .attrs = {.foreground = default_foreground,
                      .background = default_background},
        };
    }

    LOG_DBG("resize: %dx%d, grid: cols=%d, rows=%d",
            c->width, c->height, c->term.grid.cols, c->term.grid.rows);

    /* Update environment variables */
    char cols_s[12], rows_s[12];
    sprintf(cols_s, "%u", c->term.grid.cols);
    sprintf(rows_s, "%u", c->term.grid.rows);
    setenv("COLUMNS", cols_s, 1);
    setenv("LINES", rows_s, 1);

    /* SIignal TIOCSWINSZ */
    if (ioctl(c->ptmx, TIOCSWINSZ,
              &(struct winsize){.ws_row = c->term.grid.rows,
                                .ws_col = c->term.grid.cols}) == -1)
    {
        LOG_ERRNO("TIOCSWINSZ");
    }

    c->term.grid.dirty = c->term.grid.all_dirty = true;

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

int
main(int argc, const char *const *argv)
{
    int ret = EXIT_FAILURE;

    setlocale(LC_ALL, "");

    struct context c = {
        .quit = false,
        .ptmx = posix_openpt(O_RDWR | O_NOCTTY),
        .term = {
            .vt = {
                .state = 1,
            },
            .grid = {.foreground = default_foreground,
                     .background = default_background},
        },
    };

    const char *font_name = "Dina:pixelsize=12";
    c.font = font_from_name(font_name);
    if (c.font == NULL)
        goto out;

    cairo_scaled_font_extents(c.font,  &c.fextents);

    LOG_DBG("font: height: %.2f, x-advance: %.2f",
            c.fextents.height, c.fextents.max_x_advance);
    assert(c.fextents.max_y_advance == 0);

    if (c.ptmx == -1) {
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
    c.term.grid.dirty = c.term.grid.all_dirty = true;
    resize(&c, default_width, default_height);

    wl_display_dispatch_pending(c.wl.display);

    pid_t pid = fork();
    switch (pid) {
    case -1:
        LOG_ERRNO("failed to fork");
        goto out;

    case 0:
        /* Child */
        slave_spawn(c.ptmx);
        assert(false);
        break;

    default:
        LOG_DBG("slave has PID %d", pid);
        break;
    }

    while (true) {
        struct pollfd fds[] = {
            {.fd = wl_display_get_fd(c.wl.display), .events = POLLIN},
            {.fd = c.ptmx, .events = POLLIN},
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
            ssize_t count = read(c.ptmx, data, sizeof(data));
            if (count < 0) {
                LOG_ERRNO("failed to read from pseudo terminal");
                break;
            }

            //LOG_DBG("%.*s", (int)count, data);

            vt_from_slave(&c.term, data, count);
            if (c.term.grid.dirty && !c.frame_is_scheduled)
                grid_render(&c);
        }

        if (fds[1].revents & POLLHUP) {
            ret = EXIT_SUCCESS;
            break;
        }
    }

out:
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

    free(c.term.grid.cells);

    if (c.font != NULL)
        cairo_scaled_font_destroy(c.font);

    if (c.ptmx != -1)
        close(c.ptmx);

    cairo_debug_reset_static_data();
    return ret;
}
