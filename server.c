#include "server.h"

#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>

#include <linux/un.h>

#define LOG_MODULE "server"
#define LOG_ENABLE_DBG 1
#include "log.h"

#include "shm.h"
#include "terminal.h"
#include "tllist.h"
#include "wayland.h"

struct client;
struct server {
    const struct config *conf;
    struct fdm *fdm;
    struct wayland *wayl;

    int fd;
    char *sock_path;

    tll(struct client *) clients;
};

struct client {
    struct server *server;
    int fd;

    struct terminal *term;
    int argc;
    char **argv;
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

    if (client->argv != NULL) {
        for (int i = 0; i < client->argc; i++)
            free(client->argv[i]);
        free(client->argv);
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
    char *term_env = NULL;

    if (events & EPOLLHUP)
        goto shutdown;

    assert(events & EPOLLIN);

    uint16_t term_env_len;
    if (recv(fd, &term_env_len, sizeof(term_env_len), 0) != sizeof(term_env_len))
        goto shutdown;

    term_env = malloc(term_env_len + 1);
    term_env[term_env_len] = '\0';
    if (term_env_len > 0) {
        if (recv(fd, term_env, term_env_len, 0) != term_env_len)
            goto shutdown;
    }

    if (recv(fd, &client->argc, sizeof(client->argc), 0) != sizeof(client->argc))
        goto shutdown;

    LOG_DBG("argc = %d", client->argc);

    client->argv = calloc(client->argc + 1, sizeof(client->argv[0]));
    for (int i = 0; i < client->argc; i++) {
        uint16_t len;
        if (recv(fd, &len, sizeof(len), 0) != sizeof(len))
            goto shutdown;

        client->argv[i] = malloc(len + 1);
        client->argv[i][len] = '\0';
        if (len == 0)
            continue;

        if (recv(fd, client->argv[i], len, 0) != len)
            goto shutdown;

        LOG_DBG("argv[%d] = %s (%hu)", i, client->argv[i], len);
    }

    assert(client->term == NULL);
    client->term = term_init(
        server->conf, server->fdm, server->wayl,
        term_env_len > 0 ? term_env : server->conf->term,
        client->argc, client->argv, &term_shutdown_handler, client);

    if (client->term == NULL) {
        LOG_ERR("failed to instantiate new terminal");
        goto shutdown;
    }

    free(term_env);
    return true;

shutdown:
    LOG_DBG("client FD=%d: disconnected", client->fd);

    free(term_env);

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
        LOG_ERR("client FD=%d: failed to add client to FDM", client_fd);
        close(client_fd);
        free(client);
        return false;
    }

    LOG_DBG("client FD=%d: connected", client_fd);
    tll_push_back(server->clients, client);
    return true;
}

static char *
get_socket_path(void)
{
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime == NULL)
        return strdup("/tmp/foot.sock");

    char *path = malloc(strlen(xdg_runtime) + 1 + strlen("foot.sock") + 1);
    sprintf(path, "%s/foot.sock", xdg_runtime);
    return path;
}

enum connect_status {CONNECT_ERR, CONNECT_FAIL, CONNECT_SUCCESS};

static enum connect_status
try_connect(const char *sock_path)
{
    enum connect_status ret = CONNECT_ERR;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        LOG_ERRNO("failed to create UNIX socket");
        goto err;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    ret = connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0
        ? CONNECT_FAIL : CONNECT_SUCCESS;

err:
    if (fd != -1)
        close(fd);
    return ret;
}

struct server *
server_init(const struct config *conf, struct fdm *fdm, struct wayland *wayl)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        LOG_ERRNO("failed to create UNIX socket");
        return NULL;
    }

    struct server *server = NULL;
    char *sock_path = NULL;

    if ((sock_path = get_socket_path()) == NULL)
        goto err;

    switch (try_connect(sock_path)) {
    case CONNECT_FAIL:
        break;

    case CONNECT_SUCCESS:
        LOG_ERR("foot --server already running");
        /* FALLTHROUGH */

    case CONNECT_ERR:
        goto err;
    }

    unlink(sock_path);

    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | FD_CLOEXEC) < 0) {
        LOG_ERRNO("failed to set FD_CLOEXEC on socket");
        goto err;
    }

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

    if (!fdm_add(fdm, fd, EPOLLIN, &fdm_server, server)) {
        LOG_ERR("failed to add server FD to the FDM");
        goto err;
    }

    return server;

err:
    free(server);
    free(sock_path);
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
    free(server->sock_path);
    free(server);
}
