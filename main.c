#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include <poll.h>

#include <wayland-client.h>
#include <xdg-shell.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "shm.h"

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
    struct wayland wl;
};

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
    LOG_DBG("global: %s", interface);
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
    LOG_DBG("xdg-toplevel: configure: %dx%d", width, height);
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
    LOG_DBG("xdg-surface: configure");
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

    struct context c = {
        .quit = false,
        .wl = {0},
    };

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
    xdg_toplevel_set_title(c.wl.xdg_toplevel, "hello world");

    wl_surface_commit(c.wl.surface);
    wl_display_roundtrip(c.wl.display);

    /* TODO: use font metrics to calculate initial size from ROWS x COLS */
    const int default_width = 300;
    const int default_height = 300;
    struct buffer *buf = shm_get_buffer(c.wl.shm, default_width, default_height);

    cairo_set_operator(buf->cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(buf->cairo, 1.0, 0.0, 0.0, 1.0);
    cairo_rectangle(buf->cairo, 0, 0, buf->width, buf->height);
    cairo_fill(buf->cairo);

    wl_surface_attach(c.wl.surface, buf->wl_buf, 0, 0);
    wl_surface_damage(c.wl.surface, 0, 0, buf->width, buf->height);


    wl_surface_commit(c.wl.surface);

    wl_display_dispatch_pending(c.wl.display);

    while (!c.quit) {
        struct pollfd fds[] = {
            {.fd = wl_display_get_fd(c.wl.display), .events = POLLIN},
        };

        LOG_DBG("polling...");
        wl_display_flush(c.wl.display);
        poll(fds, 1, -1);
        LOG_DBG("lsdjfldsf");

        if (fds[0].revents & POLLHUP) {
            LOG_WARN("disconnected from wayland");
            break;
        }

        wl_display_dispatch(c.wl.display);
    }

    ret = EXIT_SUCCESS;

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

    cairo_debug_reset_static_data();
    return ret;
}
