#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/un.h>

#define LOG_MODULE "foot-client"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "client-protocol.h"
#include "foot-features.h"
#include "version.h"
#include "xmalloc.h"

static volatile sig_atomic_t aborted = 0;

static void
sig_handler(int signo)
{
    aborted = 1;
}

static const char *
version_and_features(void)
{
    static char buf[256];
    snprintf(buf, sizeof(buf), "version: %s %cime",
             FOOT_VERSION, feature_ime() ? '+' : '-');
    return buf;
}

static void
print_usage(const char *prog_name)
{
    printf("Usage: %s [OPTIONS...]", prog_name);
    printf("Usage: %s [OPTIONS...] [ARGS...]\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -t,--term=TERM                        value to set the environment variable TERM to (foot)\n"
           "     --title=TITLE                      initial window title (foot)\n"
           "  -a,--app-id=ID                        window application ID (foot)\n"
           "  -w,--window-size-pixels=WIDTHxHEIGHT  initial width and height, in pixels\n"
           "  -W,--window-size-chars=WIDTHxHEIGHT   initial width and height, in characters\n"
           "     --maximized                        start in maximized mode\n"
           "     --fullscreen                       start in fullscreen mode\n"
           "     --login-shell                      start shell as a login shell\n"
           "  -s,--server-socket=PATH               path to the server UNIX domain socket (default=$XDG_RUNTIME_DIR/foot-$WAYLAND_DISPLAY.sock)\n"
           "     --hold                             remain open after child process exits\n"
           "  -l,--log-colorize=[never|always|auto] enable/disable colorization of log output on stderr\n"
           "  -v,--version                          show the version number and quit\n");
}

int
main(int argc, char *const *argv)
{
    int ret = EXIT_FAILURE;

    const char *const prog_name = argv[0];

    static const struct option longopts[] =  {
        {"term",               required_argument, NULL, 't'},
        {"title",              required_argument, NULL, 'T'},
        {"app-id",             required_argument, NULL, 'a'},
        {"window-size-pixels", required_argument, NULL, 'w'},
        {"window-size-chars",  required_argument, NULL, 'W'},
        {"maximized",          no_argument,       NULL, 'm'},
        {"fullscreen",         no_argument,       NULL, 'F'},
        {"login-shell",        no_argument,       NULL, 'L'},
        {"server-socket",      required_argument, NULL, 's'},
        {"hold",               no_argument,       NULL, 'H'},
        {"log-colorize",       optional_argument, NULL, 'l'},
        {"version",            no_argument,       NULL, 'v'},
        {"help",               no_argument,       NULL, 'h'},
        {NULL,                 no_argument,       NULL,   0},
    };

    const char *term = "";
    const char *title = "";
    const char *app_id = "";
    unsigned size_type = 0; // enum conf_size_type (without pulling in tllist/fcft via config.h)
    unsigned width = 0;
    unsigned height = 0;
    const char *server_socket_path = NULL;
    enum log_colorize log_colorize = LOG_COLORIZE_AUTO;
    bool login_shell = false;
    bool maximized = false;
    bool fullscreen = false;
    bool hold = false;

    while (true) {
        int c = getopt_long(argc, argv, "+t:T:a:w:W:mFLs:Hl::vh", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 't':
            term = optarg;
            break;

        case 'T':
            title = optarg;
            break;

        case 'a':
            app_id = optarg;
            break;

        case 'L':
            login_shell = true;
            break;

        case 'w':
            if (sscanf(optarg, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
                fprintf(stderr, "error: invalid window-size-pixels: %s\n", optarg);
                return EXIT_FAILURE;
            }
            size_type = 0; // CONF_SIZE_PX
            break;

        case 'W':
            if (sscanf(optarg, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
                fprintf(stderr, "error: invalid window-size-chars: %s\n", optarg);
                return EXIT_FAILURE;
            }
            size_type = 1; // CONF_SIZE_CELLS
            break;

        case 'm':
            maximized = true;
            fullscreen = false;
            break;

        case 'F':
            fullscreen = true;
            maximized = false;
            break;

        case 's':
            server_socket_path = optarg;
            break;

        case 'H':
            hold = true;
            break;

        case 'l':
            if (optarg == NULL || strcmp(optarg, "auto") == 0)
                log_colorize = LOG_COLORIZE_AUTO;
            else if (strcmp(optarg, "never") == 0)
                log_colorize = LOG_COLORIZE_NEVER;
            else if (strcmp(optarg, "always") == 0)
                log_colorize = LOG_COLORIZE_ALWAYS;
            else {
                fprintf(stderr, "%s: argument must be one of 'never', 'always' or 'auto'\n", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'v':
            printf("footclient %s\n", version_and_features());
            return EXIT_SUCCESS;

        case 'h':
            print_usage(prog_name);
            return EXIT_SUCCESS;

        case '?':
            return EXIT_FAILURE;
        }
    }

    argc -= optind;
    argv += optind;

    log_init(log_colorize, false, LOG_FACILITY_USER, LOG_CLASS_WARNING);

    /* malloc:ed and needs to be in scope of all goto's */
    char *cwd = NULL;
    struct client_argv *cargv = NULL;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        LOG_ERRNO("failed to create socket");
        goto err;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};

    if (server_socket_path != NULL) {
        strncpy(addr.sun_path, server_socket_path, sizeof(addr.sun_path) - 1);
        if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
            LOG_ERR("%s: failed to connect (is 'foot --server' running?)", server_socket_path);
            goto err;
        }
    } else {
        bool connected = false;

        const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
        if (xdg_runtime != NULL) {
            const char *wayland_display = getenv("WAYLAND_DISPLAY");
            if (wayland_display != NULL)
                snprintf(addr.sun_path, sizeof(addr.sun_path),
                         "%s/foot-%s.sock", xdg_runtime, wayland_display);
            else
                snprintf(addr.sun_path, sizeof(addr.sun_path),
                         "%s/foot.sock", xdg_runtime);

            if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0)
                connected = true;
            else
                LOG_WARN("%s: failed to connect, will now try /tmp/foot.sock", addr.sun_path);
        }

        if (!connected) {
            strncpy(addr.sun_path, "/tmp/foot.sock", sizeof(addr.sun_path) - 1);
            if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
                LOG_ERRNO("failed to connect (is 'foot --server' running?)");
                goto err;
            }
        }
    }

    {
        errno = 0;
        size_t buf_len = 1024;
        do {
            cwd = xrealloc(cwd, buf_len);
            if (getcwd(cwd, buf_len) == NULL && errno != ERANGE) {
                LOG_ERRNO("failed to get current working directory");
                goto err;
            }
            buf_len *= 2;
        } while (errno == ERANGE);
    }

    /* String lengths, including NULL terminator */
    const size_t cwd_len = strlen(cwd) + 1;
    const size_t term_len = strlen(term) + 1;
    const size_t title_len = strlen(title) + 1;
    const size_t app_id_len = strlen(app_id) + 1;

    const struct client_data data = {
        .width = width,
        .height = height,
        .size_type = size_type,
        .maximized = maximized,
        .fullscreen = fullscreen,
        .hold = hold,
        .login_shell = login_shell,
        .cwd_len = cwd_len,
        .term_len = term_len,
        .title_len = title_len,
        .app_id_len = app_id_len,
        .argc = argc,
    };

    /* Total packet length, not (yet) including argv[] */
    size_t total_len = (
        sizeof(data) +
        cwd_len +
        term_len +
        title_len +
        app_id_len);

    cargv = xmalloc(argc * sizeof(cargv[0]));

    /* Add argv[] size to total packet length */
    for (size_t i = 0; i < argc; i++) {
        const size_t arg_len = strlen(argv[i]) + 1;

        if (arg_len >= 1 << (8 * sizeof(cargv[i].len))) {
            LOG_ERR("argv length overflow");
            goto err;
        }

        cargv[i].len = arg_len;
        total_len += sizeof(cargv[i]) + cargv[i].len;
    }

    /* Check for size overflows */
    if (total_len >= 1llu << (8 * sizeof(uint32_t)) ||
        cwd_len >= 1 << (8 * sizeof(data.cwd_len)) ||
        term_len >= 1 << (8 * sizeof(data.term_len)) ||
        title_len >= 1 << (8 * sizeof(data.title_len)) ||
        app_id_len >= 1 << (8 * sizeof(data.app_id_len)) ||
        argc > (int)(unsigned int)data.argc)
    {
        LOG_ERR("size overflow");
        goto err;
    }

    /* Send everything except argv[] */
    if (send(fd, &(uint32_t){total_len}, sizeof(uint32_t), 0) != sizeof(uint32_t) ||
        send(fd, &data, sizeof(data), 0) != sizeof(data) ||
        send(fd, cwd, cwd_len, 0) != cwd_len ||
        send(fd, term, term_len, 0) != term_len ||
        send(fd, title, title_len, 0) != title_len ||
        send(fd, app_id, app_id_len, 0) != app_id_len)
    {
        LOG_ERRNO("failed to send setup packet to server");
        goto err;
    }

    /* Send argv[] */
    for (size_t i = 0; i < argc; i++) {
        if (send(fd, &cargv[i], sizeof(cargv[i]), 0) != sizeof(cargv[i]) ||
            send(fd, argv[i], cargv[i].len, 0) != cargv[i].len)
        {
            LOG_ERRNO("failed to send setup packet to server");
            goto err;
        }
    }

    const struct sigaction sa = {.sa_handler = &sig_handler};
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        LOG_ERRNO("failed to register signal handlers");
        goto err;
    }

    int exit_code;
    ssize_t rcvd = recv(fd, &exit_code, sizeof(exit_code), 0);

    if (rcvd == -1 && errno == EINTR)
        assert(aborted);
    else if (rcvd != sizeof(exit_code))
        LOG_ERRNO("failed to read server response");
    else
        ret = exit_code;

err:
    free(cargv);
    free(cwd);
    if (fd != -1)
        close(fd);
    log_deinit();
    return ret;
}
