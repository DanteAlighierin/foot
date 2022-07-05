#include "server.h"

#include <string.h>
#include <fcntl.h>
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
#include "util.h"
#include "wayland.h"
#include "xmalloc.h"

struct client;
struct terminal_instance;

struct server {
    const struct config *conf;
    struct fdm *fdm;
    struct reaper *reaper;
    struct wayland *wayl;

    int fd;
    const char *sock_path;

    tll(struct client *) clients;
    tll(struct terminal_instance *) terminals;
};

struct client {
    struct server *server;
    int fd;

    struct {
        uint8_t *data;
        size_t left;
        size_t idx;
    } buffer;

    struct terminal_instance *instance;
};
static void client_destroy(struct client *client);

struct terminal_instance {
    struct terminal *terminal;
    struct server *server;
    struct client *client;
    struct config *conf;
};
static void instance_destroy(struct terminal_instance *instance, int exit_code);

static void
client_destroy(struct client *client)
{
    if (client == NULL)
        return;

    if (client->instance != NULL) {
        LOG_WARN("client FD=%d: terminal still alive", client->fd);
        client->instance->client = NULL;
        instance_destroy(client->instance, 1);
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
instance_destroy(struct terminal_instance *instance, int exit_code)
{
    if (instance->terminal != NULL)
        term_destroy(instance->terminal);

    tll_foreach(instance->server->terminals, it) {
        if (it->item == instance) {
            tll_remove(instance->server->terminals, it);
            break;
        }
    }

    if (instance->client != NULL) {
        instance->client->instance = NULL;
        client_send_exit_code(instance->client, exit_code);
        client_destroy(instance->client);
    }

    /* TODO: clone server conf completely, so that we can just call
     * conf_destroy() here */
    if (instance->conf != NULL) {
        config_free(instance->conf);
        free(instance->conf);
    }
    free(instance);

}

static void
term_shutdown_handler(void *data, int exit_code)
{
    struct terminal_instance *instance = data;

    instance->terminal = NULL;
    instance_destroy(instance, exit_code);
}

static bool
fdm_client(struct fdm *fdm, int fd, int events, void *data)
{
    struct client *client = data;
    struct server *server = client->server;

    char **argv = NULL;
    config_override_t overrides = tll_init();
    char **envp = NULL;

    if (events & EPOLLHUP)
        goto shutdown;

    xassert(events & EPOLLIN);

    if (client->instance != NULL) {
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

    xassert(client->instance == NULL);
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

    /* XDGA token */
    const char *token = NULL;
    if (cdata.xdga_token) {

        CHECK_BUF_AND_NULL(cdata.token_len);
        token = (const char *)p; p += cdata.token_len;
        LOG_DBG("XDGA = %.*s", cdata.token_len, token);
    } else {
        LOG_DBG("No XDGA token");
    }

    /* Overrides */
    for (uint16_t i = 0; i < cdata.override_count; i++) {
        struct client_string arg;
        CHECK_BUF(sizeof(arg));
        memcpy(&arg, p, sizeof(arg)); p += sizeof(arg);

        CHECK_BUF_AND_NULL(arg.len);
        const char *str = (const char *)p;
        p += arg.len;

        tll_push_back(overrides, xstrdup(str));
    }

    /* argv */
    argv = xcalloc(cdata.argc + 1, sizeof(argv[0]));
    for (uint16_t i = 0; i < cdata.argc; i++) {
        struct client_string arg;
        CHECK_BUF(sizeof(arg));
        memcpy(&arg, p, sizeof(arg)); p += sizeof(arg);

        CHECK_BUF_AND_NULL(arg.len);
        argv[i] = (char *)p; p += arg.len;
        LOG_DBG("argv[%hu] = %.*s", i, arg.len, argv[i]);
    }

    /* envp */
    envp = cdata.env_count != 0
        ? xcalloc(cdata.env_count + 1, sizeof(envp[0]))
        : NULL;

    for (uint16_t i = 0; i < cdata.env_count; i++) {
        struct client_string e;
        CHECK_BUF(sizeof(e));
        memcpy(&e, p, sizeof(e)); p += sizeof(e);

        CHECK_BUF_AND_NULL(e.len);
        envp[i] = (char *)p; p += e.len;
        LOG_DBG("env[%hu] = %.*s", i, e.len, envp[i]);
    }

#undef CHECK_BUF_AND_NULL
#undef CHECK_BUF

    struct terminal_instance *instance = malloc(sizeof(struct terminal_instance));

    const bool need_to_clone_conf =
        tll_length(overrides)> 0 ||
        cdata.hold != server->conf->hold_at_exit;

    struct config *conf = NULL;
    if (need_to_clone_conf) {
        conf = config_clone(server->conf);

        if (cdata.hold != server->conf->hold_at_exit)
            conf->hold_at_exit = cdata.hold;

        config_override_apply(conf, &overrides, false);

        if (conf->tweak.font_monospace_warn && conf->fonts[0].count > 0) {
            check_if_font_is_monospaced(
                conf->fonts[0].arr[0].pattern,
                &conf->notifications);
        }
    }

    *instance = (struct terminal_instance) {
        .client = NULL,
        .server = server,
        .conf = conf,
    };

    instance->terminal = term_init(
        conf != NULL ? conf : server->conf,
        server->fdm, server->reaper, server->wayl, "footclient", cwd, token,
        cdata.argc, argv, envp, &term_shutdown_handler, instance);

    if (instance->terminal == NULL) {
        LOG_ERR("failed to instantiate new terminal");
        client_send_exit_code(client, -26);
        instance_destroy(instance, -1);
        goto shutdown;
    }

    if (cdata.no_wait) {
        // the server owns the instance
        tll_push_back(server->terminals, instance);
        client_send_exit_code(client, 0);
        goto shutdown;
    } else {
        // the instance is attached to the client
        instance->client = client;
        client->instance = instance;
        free(argv);
        free(envp);
        tll_free_and_free(overrides, free);
    }

    return true;

shutdown:
    LOG_DBG("client FD=%d: disconnected", client->fd);

    free(argv);
    free(envp);
    tll_free_and_free(overrides, free);
    fdm_del(fdm, fd);
    client->fd = -1;

    if (client->instance != NULL &&
        !client->instance->terminal->shutdown.in_progress)
    {
        term_shutdown(client->instance->terminal);
    } else
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

static bool
prepare_socket(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        LOG_ERRNO("failed to get file descriptors flag for passed socket");
        return false;
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        LOG_ERRNO("failed to set FD_CLOEXEC for passed socket");
        return false;
    }

    flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        LOG_ERRNO("failed to get file status flags for passed socket");
        return false;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        LOG_ERRNO("failed to set non-blocking mode on passed socket");
        return false;
    }

    int const socket_options[] = { SO_DOMAIN, SO_ACCEPTCONN, SO_TYPE };
    int const socket_options_values[] = { AF_UNIX, 1, SOCK_STREAM};
    char const * const socket_options_names[] = { "SO_DOMAIN", "SO_ACCEPTCONN", "SO_TYPE" };

    xassert(ALEN(socket_options) == ALEN(socket_options_values));
    xassert(ALEN(socket_options) == ALEN(socket_options_names));

    int socket_option = 0;
    socklen_t len;
    for (size_t i = 0; i < ALEN(socket_options) ; i++) {
        len = sizeof(socket_option);
        if (getsockopt(fd, SOL_SOCKET, socket_options[i], &socket_option, &len) == -1 ||
                len != sizeof(socket_option)) {
            LOG_ERRNO("failed to read socket option from passed file descriptor");
            return false;
        }
        if (socket_option != socket_options_values[i]) {
            LOG_ERR("wrong socket value for socket option '%s' on passed file descriptor",
                    socket_options_names[i]);
            return false;
        }
    }

    return true;
}

struct server *
server_init(const struct config *conf, struct fdm *fdm, struct reaper *reaper,
            struct wayland *wayl)
{
    int fd;
    struct server *server = NULL;
    const char *sock_path = conf->server_socket_path;
    char *end;

    errno = 0;
    fd = strtol(sock_path, &end, 10);
    if (*end == '\0' && *sock_path != '\0')
    {
        if (!prepare_socket(fd))
            goto err;
        LOG_DBG("we've been started by socket activation, using passed socket");
        sock_path = NULL;
    }
    else {
        LOG_DBG("no suitable pre-existing socket found, creating our own");
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd == -1) {
            LOG_ERRNO("failed to create UNIX socket");
            return NULL;
        }

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
        .terminals = tll_init(),
    };

    if (!fdm_add(fdm, fd, EPOLLIN, &fdm_server, server))
        goto err;

    LOG_INFO("accepting connections on %s", sock_path != NULL ? sock_path : "socket provided through socket activation");

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
        client_send_exit_code(it->item, -26);
        client_destroy(it->item);
    }

    tll_free(server->clients);

    tll_foreach(server->terminals, it)
        instance_destroy(it->item, 1);

    tll_free(server->terminals);

    fdm_del(server->fdm, server->fd);
    if (server->sock_path != NULL)
        unlink(server->sock_path);
    free(server);
}
