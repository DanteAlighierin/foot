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

struct client;
struct server {
    const struct config *conf;
    struct fdm *fdm;
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

    shm_purge(client->server->wayl->shm,
              (unsigned long)(uintptr_t)client->term);

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
        if (recv(fd, &total_len, sizeof(total_len), 0) != sizeof(total_len)) {
            LOG_ERRNO("failed to read total length");
            goto shutdown;
        }

        LOG_DBG("total len: %u", total_len);
        client->buffer.data = malloc(total_len + 1);
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

    CHECK_BUF(sizeof(uint16_t));
    uint16_t term_env_len = *(uint16_t *)p; p += sizeof(term_env_len);

    CHECK_BUF(term_env_len);
    const char *term_env = (const char *)p; p += strlen(term_env) + 1;
    LOG_DBG("TERM = %.*s", term_env_len, term_env);

    if (term_env_len != strlen(term_env) + 1) {
        LOG_ERR("TERM length mismatch: indicated = %hu, actual = %zu",
                term_env_len - 1, strlen(term_env));
        goto shutdown;
    }

    CHECK_BUF(sizeof(argc));
    argc = *(int *)p; p += sizeof(argc);
    argv = calloc(argc + 1, sizeof(argv[0]));
    LOG_DBG("argc = %d", argc);

    for (int i = 0; i < argc; i++) {
        CHECK_BUF(sizeof(uint16_t));
        uint16_t len = *(uint16_t *)p; p += sizeof(len);

        CHECK_BUF(len);
        argv[i] = (char *)p; p += strlen(argv[i]) + 1;
        LOG_DBG("argv[%d] = %s", i, argv[i]);

        if (len != strlen(argv[i]) + 1) {
            LOG_ERR("argv[%d] length mismatch: indicated = %hu, actual = %zu",
                    i, len - 1, strlen(argv[i]));
            goto shutdown;
        }
    }
    argv[argc] = NULL;

#undef CHECK_BUF

    client->term = term_init(
        server->conf, server->fdm, server->wayl,
        strlen(term_env) > 0 ? term_env : server->conf->term,
        "footclient" /* TODO */, argc, argv, &term_shutdown_handler, client);

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
        server->fd, (struct sockaddr *)&addr, &addr_size, SOCK_CLOEXEC);

    if (client_fd == -1) {
        LOG_ERRNO("failed to accept client connection");
        return false;
    }

    struct client *client = malloc(sizeof(*client));
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
server_init(const struct config *conf, struct fdm *fdm, struct wayland *wayl)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
    *server = (struct server) {
        .conf = conf,
        .fdm = fdm,
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
