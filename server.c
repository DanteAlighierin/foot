#include "server.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include <sys/un.h>

#include <tllist.h>

#define LOG_MODULE "server"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "client-protocol.h"
#include "shm.h"
#include "terminal.h"
#include "wayland.h"
#include "xmalloc.h"

struct client;
struct server {
    const struct config *conf;
    struct fdm *fdm;
    struct reaper *reaper;
    struct wayland *wayl;

    int fd;
    const char *sock_path;

    tll(struct client *) clients;
};

struct client {
    struct server *server;
    int fd;

    struct {
        uint8_t *data;
        size_t left;
        size_t idx;
    } buffer;

    struct config conf;
    struct terminal *term;
};

static void
client_destroy(struct client *client)
{
    if (client == NULL)
        return;

    if (client->term != NULL) {
        LOG_WARN("client FD=%d: terminal still alive", client->fd);
        term_destroy(client->term);
    }

    if (client->fd != -1) {
        LOG_DBG("client FD=%d: disconnected", client->fd);
        fdm_del(client->server->fdm, client->fd);
    }

    tll_foreach(client->server->clients, it) {
        if (it->item == client) {
            tll_remove(client->server->clients, it);
            break;
        }
    }

    free(client->buffer.data);

    /* TODO: clone server conf completely, so that we can just call
     * conf_destroy() here */
    free(client->conf.term);
    free(client->conf.title);
    free(client->conf.app_id);

    free(client);
}

static void
client_send_exit_code(struct client *client, int exit_code)
{
    if (client->fd == -1)
        return;

    if (write(client->fd, &exit_code, sizeof(exit_code)) != sizeof(exit_code))
        LOG_ERRNO("failed to write slave exit code to client");
}

static void
term_shutdown_handler(void *data, int exit_code)
{
    struct client *client = data;

    struct wl_shm *shm = client->server->wayl->shm;
    struct terminal *term = client->term;

    shm_purge(shm, shm_cookie_grid(term));
    shm_purge(shm, shm_cookie_search(term));
    for (enum csd_surface surf = 0; surf < CSD_SURF_COUNT; surf++)
        shm_purge(shm, shm_cookie_csd(term, surf));

    client_send_exit_code(client, exit_code);

    client->term = NULL;
    client_destroy(client);
}

static bool
fdm_client(struct fdm *fdm, int fd, int events, void *data)
{
    struct client *client = data;
    struct server *server = client->server;

    char **argv = NULL;

    if (events & EPOLLHUP)
        goto shutdown;

    xassert(events & EPOLLIN);

    if (client->term != NULL) {
        uint8_t dummy[128];
        ssize_t count = read(fd, dummy, sizeof(dummy));
        LOG_WARN("client unexpectedly sent %zd bytes", count);
        return true;  /* TODO: shutdown instead? */
    }

    if (client->buffer.data == NULL) {
        /*
         * We haven't received any data yet - the first thing the
         * client sends is the total size of the initialization
         * data.
         */
        uint32_t total_len;
        ssize_t count = recv(fd, &total_len, sizeof(total_len), 0);
        if (count < 0) {
            LOG_ERRNO("failed to read total length");
            goto shutdown;
        }

        if (count != sizeof(total_len)) {
            LOG_ERR("client did not send setup packet size");
            goto shutdown;
        }

        const uint32_t max_size = 128 * 1024;
        if (total_len > max_size) {
            LOG_ERR("client wants to send too large setup packet (%u > %u)",
                    total_len, max_size);
            goto shutdown;
        }

        LOG_DBG("total len: %u", total_len);
        client->buffer.data = xmalloc(total_len + 1);
        client->buffer.left = total_len;
        client->buffer.idx = 0;

        /* Prevent our strlen() calls to run outside */
        client->buffer.data[total_len] = '\0';
        return true;  /* Let FDM trigger again when we have more data */
    }

    /* Keep filling our buffer of initialization data */
    ssize_t count = recv(
        fd, &client->buffer.data[client->buffer.idx], client->buffer.left, 0);

    if (count < 0) {
        LOG_ERRNO("failed to read");
        goto shutdown;
    }

    client->buffer.idx += count;
    client->buffer.left -= count;

    if (client->buffer.left > 0) {
        /* Not done yet */
        return true;
    }

    /* All initialization data received - time to instantiate a terminal! */

    xassert(client->term == NULL);
    xassert(client->buffer.data != NULL);
    xassert(client->buffer.left == 0);

    /*
     * Parse the received buffer, verifying lengths etc
     */

#define CHECK_BUF(sz) do {                      \
        if (p + (sz) > end)                     \
            goto shutdown;                      \
    } while (0)

#define CHECK_BUF_AND_NULL(sz) do {             \
        CHECK_BUF(sz);                          \
        if (sz == 0)                            \
            goto shutdown;                      \
        if (p[sz - 1] != '\0')                  \
            goto shutdown;                      \
    } while (0)

    uint8_t *p = client->buffer.data;
    const uint8_t *end = &client->buffer.data[client->buffer.idx];

    struct client_data cdata;
    CHECK_BUF(sizeof(cdata));
    memcpy(&cdata, p, sizeof(cdata));
    p += sizeof(cdata);

    CHECK_BUF_AND_NULL(cdata.cwd_len);
    const char *cwd = (const char *)p; p += cdata.cwd_len;
    LOG_DBG("CWD = %.*s", cdata.cwd_len, cwd);

    CHECK_BUF_AND_NULL(cdata.term_len);
    const char *term_env = (const char *)p; p += cdata.term_len;
    LOG_DBG("TERM = %.*s", cdata.term_len, term_env);

    CHECK_BUF_AND_NULL(cdata.title_len);
    const char *title = (const char *)p; p += cdata.title_len;
    LOG_DBG("title = %.*s", cdata.title_len, title);

    CHECK_BUF_AND_NULL(cdata.app_id_len);
    const char *app_id = (const char *)p; p += cdata.app_id_len;
    LOG_DBG("app-id = %.*s", cdata.app_id_len, app_id);

    argv = xcalloc(cdata.argc + 1, sizeof(argv[0]));

    for (uint16_t i = 0; i < cdata.argc; i++) {
        struct client_argv arg;
        CHECK_BUF(sizeof(arg));
        memcpy(&arg, p, sizeof(arg));
        p += sizeof(arg);

        CHECK_BUF_AND_NULL(arg.len);
        argv[i] = (char *)p; p += arg.len;
        LOG_DBG("argv[%hu] = %.*s", i, arg.len, argv[i]);
    }
    argv[cdata.argc] = NULL;

#undef CHECK_BUF_AND_NULL
#undef CHECK_BUF

    client->conf = *server->conf;
    client->conf.term = strlen(term_env) > 0
        ? xstrdup(term_env) : xstrdup(server->conf->term);
    client->conf.title = strlen(title) > 0
        ? xstrdup(title) : xstrdup(server->conf->title);
    client->conf.app_id = strlen(app_id) > 0
        ? xstrdup(app_id) : xstrdup(server->conf->app_id);
    client->conf.hold_at_exit = cdata.hold;
    client->conf.login_shell = cdata.login_shell;

    if (cdata.maximized)
        client->conf.startup_mode = STARTUP_MAXIMIZED;
    else if (cdata.fullscreen)
        client->conf.startup_mode = STARTUP_FULLSCREEN;

    if (cdata.width > 0 && cdata.height > 0) {
        client->conf.size.type = cdata.size_type;
        client->conf.size.width = cdata.width;
        client->conf.size.height = cdata.height;
    }

    client->term = term_init(
        &client->conf, server->fdm, server->reaper, server->wayl,
        "footclient", cwd, cdata.argc, argv, &term_shutdown_handler, client);

    if (client->term == NULL) {
        LOG_ERR("failed to instantiate new terminal");
        client_send_exit_code(client, -1);
        goto shutdown;
    }

    free(argv);
    return true;

shutdown:
    LOG_DBG("client FD=%d: disconnected", client->fd);

    free(argv);
    fdm_del(fdm, fd);
    client->fd = -1;

    if (client->term != NULL && !client->term->is_shutting_down)
        term_shutdown(client->term);
    else
        client_destroy(client);

    return true;
}

static bool
fdm_server(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct server *server = data;

    struct sockaddr_un addr;
    socklen_t addr_size = sizeof(addr);
    int client_fd = accept4(
        server->fd, (struct sockaddr *)&addr, &addr_size, SOCK_CLOEXEC | SOCK_NONBLOCK);

    if (client_fd == -1) {
        LOG_ERRNO("failed to accept client connection");
        return false;
    }

    struct client *client = xmalloc(sizeof(*client));
    *client = (struct client) {
        .server = server,
        .fd = client_fd,
    };

    if (!fdm_add(server->fdm, client_fd, EPOLLIN, &fdm_client, client)) {
        close(client_fd);
        free(client);
        return false;
    }

    LOG_DBG("client FD=%d: connected", client_fd);
    tll_push_back(server->clients, client);
    return true;
}

enum connect_status {CONNECT_ERR, CONNECT_FAIL, CONNECT_SUCCESS};

static enum connect_status
try_connect(const char *sock_path)
{
    enum connect_status ret = CONNECT_ERR;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd == -1) {
        LOG_ERRNO("failed to create UNIX socket");
        goto err;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    switch (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    case 0:
        ret = CONNECT_SUCCESS;
        break;

    case -1:
        LOG_DBG("connect() failed: %s", strerror(errno));
        ret = CONNECT_FAIL;
        break;
    }

err:
    if (fd != -1)
        close(fd);
    return ret;
}

struct server *
server_init(const struct config *conf, struct fdm *fdm, struct reaper *reaper,
            struct wayland *wayl)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd == -1) {
        LOG_ERRNO("failed to create UNIX socket");
        return NULL;
    }

    struct server *server = NULL;
    const char *sock_path = conf->server_socket_path;

    switch (try_connect(sock_path)) {
    case CONNECT_FAIL:
        break;

    case CONNECT_SUCCESS:
        LOG_ERR("%s is already accepting connections; is 'foot --server' already running", sock_path);
        /* FALLTHROUGH */

    case CONNECT_ERR:
        goto err;
    }

    unlink(sock_path);

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERRNO("%s: failed to bind", addr.sun_path);
        goto err;
    }

    if (listen(fd, 0) < 0) {
        LOG_ERRNO("%s: failed to listen", addr.sun_path);
        goto err;
    }

    server = malloc(sizeof(*server));
    if (unlikely(server == NULL)) {
        LOG_ERRNO("malloc() failed");
        goto err;
    }

    *server = (struct server) {
        .conf = conf,
        .fdm = fdm,
        .reaper = reaper,
        .wayl = wayl,

        .fd = fd,
        .sock_path = sock_path,

        .clients = tll_init(),
    };

    if (!fdm_add(fdm, fd, EPOLLIN, &fdm_server, server))
        goto err;

    LOG_INFO("accepting connections on %s", sock_path);

    return server;

err:
    free(server);
    if (fd != -1)
        close(fd);
    return NULL;
}

void
server_destroy(struct server *server)
{
    if (server == NULL)
        return;

    LOG_DBG("server destroy, %zu clients still alive",
            tll_length(server->clients));

    tll_foreach(server->clients, it) {
        client_send_exit_code(it->item, 1);
        client_destroy(it->item);
    }

    tll_free(server->clients);

    fdm_del(server->fdm, server->fd);
    if (server->sock_path != NULL)
        unlink(server->sock_path);
    free(server);
}
