#include "server.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include <linux/un.h>

#include <tllist.h>

#define LOG_MODULE "server"
#define LOG_ENABLE_DBG 0
#include "log.h"

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
    int argc = 0;

    if (events & EPOLLHUP)
        goto shutdown;

    assert(events & EPOLLIN);

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

    assert(client->term == NULL);
    assert(client->buffer.data != NULL);
    assert(client->buffer.left == 0);

    /*
     * Parse the received buffer, verifying lengths etc
     */

#define CHECK_BUF(sz) do {                      \
        if (p + (sz) > end)                     \
            goto shutdown;                      \
    } while (0)

    uint8_t *p = client->buffer.data;
    const uint8_t *end = &client->buffer.data[client->buffer.idx];

    uint16_t cwd_len;
    CHECK_BUF(sizeof(cwd_len));
    memcpy(&cwd_len, p, sizeof(cwd_len));
    p += sizeof(cwd_len);

    CHECK_BUF(cwd_len);
    const char *cwd = (const char *)p; p += cwd_len;
    LOG_DBG("CWD = %.*s", cwd_len, cwd);

    if (cwd_len != strlen(cwd) + 1) {
        LOG_ERR("CWD length mismatch: indicated = %u, actual = %zu",
                cwd_len - 1, strlen(cwd));
        goto shutdown;
    }

    uint16_t term_env_len;
    CHECK_BUF(sizeof(term_env_len));
    memcpy(&term_env_len, p, sizeof(term_env_len));
    p += sizeof(term_env_len);

    CHECK_BUF(term_env_len);
    const char *term_env = (const char *)p; p += term_env_len;
    LOG_DBG("TERM = %.*s", term_env_len, term_env);

    if (term_env_len != strlen(term_env) + 1) {
        LOG_ERR("TERM length mismatch: indicated = %u, actual = %zu",
                term_env_len - 1, strlen(term_env));
        goto shutdown;
    }

    uint16_t title_len;
    CHECK_BUF(sizeof(title_len));
    memcpy(&title_len, p, sizeof(title_len));
    p += sizeof(title_len);

    CHECK_BUF(title_len);
    const char *title = (const char *)p; p += title_len;
    LOG_DBG("app-id = %.*s", title_len, title);

    if (title_len != strlen(title) + 1) {
        LOG_ERR("title length mismatch: indicated = %u, actual = %zu",
                title_len - 1, strlen(title));
        goto shutdown;
    }

    uint16_t app_id_len;
    CHECK_BUF(sizeof(app_id_len));
    memcpy(&app_id_len, p, sizeof(app_id_len));
    p += sizeof(app_id_len);

    CHECK_BUF(app_id_len);
    const char *app_id = (const char *)p; p += app_id_len;
    LOG_DBG("app-id = %.*s", app_id_len, app_id);

    if (app_id_len != strlen(app_id) + 1) {
        LOG_ERR("app-id length mismatch: indicated = %u, actual = %zu",
                app_id_len - 1, strlen(app_id));
        goto shutdown;
    }

    CHECK_BUF(sizeof(uint8_t));
    const uint8_t maximized = *(const uint8_t *)p; p += sizeof(maximized);

    CHECK_BUF(sizeof(uint8_t));
    const uint8_t fullscreen = *(const uint8_t *)p; p += sizeof(fullscreen);

    CHECK_BUF(sizeof(uint8_t));
    const uint8_t hold = *(const uint8_t *)p; p += sizeof(hold);

    CHECK_BUF(sizeof(uint8_t));
    const uint8_t login_shell = *(const uint8_t *)p; p += sizeof(login_shell);

    CHECK_BUF(sizeof(argc));
    memcpy(&argc, p, sizeof(argc));
    p += sizeof(argc);

    argv = xcalloc(argc + 1, sizeof(argv[0]));
    LOG_DBG("argc = %d", argc);

    for (int i = 0; i < argc; i++) {
        uint16_t len;
        CHECK_BUF(sizeof(len));
        memcpy(&len, p, sizeof(len));
        p += sizeof(len);

        CHECK_BUF(len);
        argv[i] = (char *)p; p += strlen(argv[i]) + 1;
        LOG_DBG("argv[%d] = %s", i, argv[i]);

        if (len != strlen(argv[i]) + 1) {
            LOG_ERR("argv[%d] length mismatch: indicated = %u, actual = %zu",
                    i, len - 1, strlen(argv[i]));
            goto shutdown;
        }
    }
    argv[argc] = NULL;

#undef CHECK_BUF

    client->conf = *server->conf;
    client->conf.term = strlen(term_env) > 0
        ? xstrdup(term_env) : xstrdup(server->conf->term);
    client->conf.title = strlen(title) > 0
        ? xstrdup(title) : xstrdup(server->conf->title);
    client->conf.app_id = strlen(app_id) > 0
        ? xstrdup(app_id) : xstrdup(server->conf->app_id);
    client->conf.hold_at_exit = hold;
    client->conf.login_shell = login_shell;

    if (maximized)
        client->conf.startup_mode = STARTUP_MAXIMIZED;
    else if (fullscreen)
        client->conf.startup_mode = STARTUP_FULLSCREEN;

    client->term = term_init(
        &client->conf, server->fdm, server->reaper, server->wayl,
        "footclient", cwd, argc, argv, &term_shutdown_handler, client);

    if (client->term == NULL) {
        LOG_ERR("failed to instantiate new terminal");
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
