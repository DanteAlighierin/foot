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
#include <linux/un.h>

#define LOG_MODULE "foot-client"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "version.h"

static volatile sig_atomic_t aborted = 0;

static void
sig_handler(int signo)
{
    aborted = 1;
}

static void
print_usage(const char *prog_name)
{
    printf("Usage: %s [OPTIONS]...\n", prog_name);
    printf("Usage: %s [OPTIONS]... -- command\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -t,--term=TERM                        value to set the environment variable TERM to (foot)\n"
           "  -a,--app-id=ID                        window application ID (foot)\n"
           "     --maximized                        start in maximized mode\n"
           "     --fullscreen                       start in fullscreen mode\n"
           "     --login-shell                      start shell as a login shell\n"
           "  -s,--server-socket=PATH               path to the server UNIX domain socket (default=$XDG_RUNTIME_DIR/foot-$XDG_SESSION_ID.sock)\n"
           "  -l,--log-colorize=[never|always|auto] enable/disable colorization of log output on stderr\n"
           "  -v,--version                          show the version number and quit\n");
}

int
main(int argc, char *const *argv)
{
    int ret = EXIT_FAILURE;

    const char *const prog_name = argv[0];

    static const struct option longopts[] =  {
        {"term",          required_argument, NULL, 't'},
        {"app-id",        required_argument, NULL, 'a'},
        {"maximized",     no_argument,       NULL, 'm'},
        {"fullscreen",    no_argument,       NULL, 'F'},
        {"login-shell",   no_argument,       NULL, 'L'},
        {"server-socket", required_argument, NULL, 's'},
        {"log-colorize",  optional_argument, NULL, 'l'},
        {"version",       no_argument,       NULL, 'v'},
        {"help",          no_argument,       NULL, 'h'},
        {NULL,            no_argument,       NULL,   0},
    };

    const char *term = "";
    const char *app_id = "";
    const char *server_socket_path = NULL;
    enum log_colorize log_colorize = LOG_COLORIZE_AUTO;
    bool login_shell = false;
    bool maximized = false;
    bool fullscreen = false;

    while (true) {
        int c = getopt_long(argc, argv, ":t:a:s:l::hv", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 't':
            term = optarg;
            break;

        case 'a':
            app_id = optarg;
            break;

        case 'L':
            login_shell = true;
            break;

        case ',':
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
            printf("footclient version %s\n", FOOT_VERSION);
            return EXIT_SUCCESS;

        case 'h':
            print_usage(prog_name);
            return EXIT_SUCCESS;

       case ':':
            fprintf(stderr, "error: -%c: missing required argument\n", optopt);
            return EXIT_FAILURE;

        case '?':
            fprintf(stderr, "error: -%c: invalid option\n", optopt);
            return EXIT_FAILURE;
        }
    }

    argc -= optind;
    argv += optind;

    log_init(log_colorize, false, LOG_FACILITY_USER, LOG_CLASS_WARNING);

    /* malloc:ed and needs to be in scope of all goto's */
    char *cwd = NULL;

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

        const char *xdg_session_id = getenv("XDG_SESSION_ID");
        const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
        if (xdg_runtime != NULL) {
            if (xdg_session_id == NULL)
                xdg_session_id = "<no-session>";

            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/foot-%s.sock", xdg_runtime, xdg_session_id);
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
            cwd = realloc(cwd, buf_len);
            if (getcwd(cwd, buf_len) == NULL && errno != ERANGE) {
                LOG_ERRNO("failed to get current working directory");
                goto err;
            }
            buf_len *= 2;
        } while (errno == ERANGE);
    }
    const uint16_t cwd_len = strlen(cwd) + 1;
    const uint16_t term_len = strlen(term) + 1;
    const uint16_t app_id_len = strlen(app_id) + 1;
    uint32_t total_len = 0;

    /* Calculate total length */
    total_len += sizeof(cwd_len) + cwd_len;
    total_len += sizeof(term_len) + term_len;
    total_len += sizeof(app_id_len) + app_id_len;
    total_len += sizeof(uint8_t);  /* maximized */
    total_len += sizeof(uint8_t);  /* fullscreen */
    total_len += sizeof(uint8_t);  /* login_shell */
    total_len += sizeof(argc);

    for (int i = 0; i < argc; i++) {
        uint16_t len = strlen(argv[i]) + 1;
        total_len += sizeof(len) + len;
    }

    LOG_DBG("term-len: %hu, argc: %d, total-len: %u",
            term_len, argc, total_len);

    if (send(fd, &total_len, sizeof(total_len), 0) != sizeof(total_len)) {
        LOG_ERRNO("failed to send total length to server");
        goto err;
    }

    if (send(fd, &cwd_len, sizeof(cwd_len), 0) != sizeof(cwd_len) ||
        send(fd, cwd, cwd_len, 0) != cwd_len)
    {
        LOG_ERRNO("failed to send CWD to server");
        goto err;
    }

    if (send(fd, &term_len, sizeof(term_len), 0) != sizeof(term_len) ||
        send(fd, term, term_len, 0) != term_len)
    {
        LOG_ERRNO("failed to send TERM to server");
        goto err;
    }

    if (send(fd, &app_id_len, sizeof(app_id_len), 0) != sizeof(app_id_len) ||
        send(fd, app_id, app_id_len, 0) != app_id_len)
    {
        LOG_ERRNO("failed to send app-id to server");
        goto err;
    }

    if (send(fd, &(uint8_t){maximized}, sizeof(uint8_t), 0) != sizeof(uint8_t)) {
        LOG_ERRNO("failed to send maximized");
        goto err;
    }

    if (send(fd, &(uint8_t){fullscreen}, sizeof(uint8_t), 0) != sizeof(uint8_t)) {
        LOG_ERRNO("failed to send fullscreen");
        goto err;
    }

    if (send(fd, &(uint8_t){login_shell}, sizeof(uint8_t), 0) != sizeof(uint8_t)) {
        LOG_ERRNO("failed to send login-shell");
        goto err;
    }

    LOG_DBG("argc = %d", argc);
    if (send(fd, &argc, sizeof(argc), 0) != sizeof(argc)) {
        LOG_ERRNO("failed to send argc/argv to server");
        goto err;
    }

    for (int i = 0; i < argc; i++) {
        uint16_t len = strlen(argv[i]) + 1;

        LOG_DBG("argv[%d] = %s (%hu)", i, argv[i], len);

        if (send(fd, &len, sizeof(len), 0) != sizeof(len) ||
            send(fd, argv[i], len, 0) != len)
        {
            LOG_ERRNO("failed to send argc/argv to server");
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
    free(cwd);
    if (fd != -1)
        close(fd);
    log_deinit();
    return ret;
}
