#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#include <tllist.h>

#define LOG_MODULE "foot-client"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "client-protocol.h"
#include "debug.h"
#include "foot-features.h"
#include "macros.h"
#include "util.h"
#include "version.h"
#include "xmalloc.h"

struct override {
    size_t len;
    char *str;
};
typedef tll(struct override) override_list_t;

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
    snprintf(buf, sizeof(buf), "version: %s %cpgo %cime %cgraphemes",
             FOOT_VERSION,
             feature_pgo() ? '+' : '-',
             feature_ime() ? '+' : '-',
             feature_graphemes() ? '+' : '-');
    return buf;
}

static void
print_usage(const char *prog_name)
{
    printf("Usage: %s [OPTIONS...]\n", prog_name);
    printf("Usage: %s [OPTIONS...] command [ARGS...]\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -t,--term=TERM                           value to set the environment variable TERM to (foot)\n"
           "  -T,--title=TITLE                         initial window title (foot)\n"
           "  -a,--app-id=ID                           window application ID (foot)\n"
           "  -w,--window-size-pixels=WIDTHxHEIGHT     initial width and height, in pixels\n"
           "  -W,--window-size-chars=WIDTHxHEIGHT      initial width and height, in characters\n"
           "  -m,--maximized                           start in maximized mode\n"
           "  -F,--fullscreen                          start in fullscreen mode\n"
           "  -L,--login-shell                         start shell as a login shell\n"
           "  -D,--working-directory=DIR               directory to start in (CWD)\n"
           "  -s,--server-socket=PATH                  path to the server UNIX domain socket (default=$XDG_RUNTIME_DIR/foot-$WAYLAND_DISPLAY.sock)\n"
           "  -H,--hold                                remain open after child process exits\n"
           "  -N,--no-wait                             detach the client process from the running terminal, exiting immediately\n"
           "  -o,--override=[section.]key=value        override configuration option\n"
           "  -d,--log-level={info|warning|error|none} log level (info)\n"
           "  -l,--log-colorize=[{never|always|auto}]  enable/disable colorization of log output on stderr\n"
           "  -v,--version                             show the version number and quit\n");
}

static bool NOINLINE
push_override(override_list_t *overrides, const char *s, uint64_t *total_len)
{
    size_t len = strlen(s) + 1;
    if (len >= 1 << (8 * sizeof(struct client_string))) {
        LOG_ERR("override length overflow");
        return false;
    }

    struct override o = {len, xstrdup(s)};
    tll_push_back(*overrides, o);
    *total_len += sizeof(struct client_string) + o.len;
    return true;
}

int
main(int argc, char *const *argv)
{
    /* Custom exit code, to enable users to differentiate between foot
     * itself failing, and the client application failiing */
    static const int foot_exit_failure = -36;
    int ret = foot_exit_failure;

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
        {"working-directory",  required_argument, NULL, 'D'},
        {"server-socket",      required_argument, NULL, 's'},
        {"hold",               no_argument,       NULL, 'H'},
        {"no-wait",            no_argument,       NULL, 'N'},
        {"override",           required_argument, NULL, 'o'},
        {"log-level",          required_argument, NULL, 'd'},
        {"log-colorize",       optional_argument, NULL, 'l'},
        {"version",            no_argument,       NULL, 'v'},
        {"help",               no_argument,       NULL, 'h'},
        {NULL,                 no_argument,       NULL,   0},
    };

    const char *custom_cwd = NULL;
    const char *server_socket_path = NULL;
    enum log_class log_level = LOG_CLASS_INFO;
    enum log_colorize log_colorize = LOG_COLORIZE_AUTO;
    bool hold = false;

    /* Used to format overrides */
    bool no_wait = false;

    char buf[1024];

    /* Total packet length, not (yet) including overrides or argv[] */
    uint64_t total_len = 0;

    /* malloc:ed and needs to be in scope of all goto's */
    int fd = -1;
    char *_cwd = NULL;
    override_list_t overrides = tll_init();
    struct client_string *cargv = NULL;

    while (true) {
        int c = getopt_long(argc, argv, "+t:T:a:w:W:mFLD:s:HNo:d:l::vh", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 't':
            snprintf(buf, sizeof(buf), "term=%s", optarg);
            if (!push_override(&overrides, buf, &total_len))
                goto err;
            break;

        case 'T':
            snprintf(buf, sizeof(buf), "title=%s", optarg);
            if (!push_override(&overrides, buf, &total_len))
                goto err;
            break;

        case 'a':
            snprintf(buf, sizeof(buf), "app-id=%s", optarg);
            if (!push_override(&overrides, buf, &total_len))
                goto err;
            break;

        case 'L':
            if (!push_override(&overrides, "login-shell=yes", &total_len))
                goto err;
            break;

        case 'D': {
            struct stat st;
            if (stat(optarg, &st) < 0 || !(st.st_mode & S_IFDIR)) {
                fprintf(stderr, "error: %s: not a directory\n", optarg);
                goto err;
            }
            custom_cwd = optarg;
            break;
        }

        case 'w': {
            unsigned width, height;
            if (sscanf(optarg, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
                fprintf(stderr, "error: invalid window-size-pixels: %s\n", optarg);
                goto err;
            }

            snprintf(buf, sizeof(buf), "initial-window-size-pixels=%ux%u", width, height);
            if (!push_override(&overrides, buf, &total_len))
                goto err;
            break;
        }

        case 'W': {
            unsigned width, height;
            if (sscanf(optarg, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
                fprintf(stderr, "error: invalid window-size-chars: %s\n", optarg);
                goto err;
            }

            snprintf(buf, sizeof(buf), "initial-window-size-chars=%ux%u", width, height);
            if (!push_override(&overrides, buf, &total_len))
                goto err;
            break;
        }

        case 'm':
            if (!push_override(&overrides, "initial-window-mode=maximized", &total_len))
                goto err;
            break;

        case 'F':
            if (!push_override(&overrides, "initial-window-mode=fullscreen", &total_len))
                goto err;
            break;

        case 's':
            server_socket_path = optarg;
            break;

        case 'H':
            hold = true;
            break;

        case 'N':
            no_wait = true;
            break;

        case 'o':
            if (!push_override(&overrides, optarg, &total_len))
                goto err;
            break;

        case 'd': {
            int lvl = log_level_from_string(optarg);
            if (unlikely(lvl < 0)) {
                fprintf(
                    stderr,
                    "-d,--log-level: %s: argument must be one of %s\n",
                    optarg,
                    log_level_string_hint());
                goto err;
            }
            log_level = lvl;
            break;
        }

        case 'l':
            if (optarg == NULL || strcmp(optarg, "auto") == 0)
                log_colorize = LOG_COLORIZE_AUTO;
            else if (strcmp(optarg, "never") == 0)
                log_colorize = LOG_COLORIZE_NEVER;
            else if (strcmp(optarg, "always") == 0)
                log_colorize = LOG_COLORIZE_ALWAYS;
            else {
                fprintf(stderr, "%s: argument must be one of 'never', 'always' or 'auto'\n", optarg);
                goto err;
            }
            break;

        case 'v':
            printf("footclient %s\n", version_and_features());
            ret = EXIT_SUCCESS;
            goto err;

        case 'h':
            print_usage(prog_name);
            ret = EXIT_SUCCESS;
            goto err;

        case '?':
            goto err;
        }
    }

    argc -= optind;
    argv += optind;

    log_init(log_colorize, false, LOG_FACILITY_USER, log_level);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
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

    const char *cwd = custom_cwd;
    if (cwd == NULL) {
        errno = 0;
        size_t buf_len = 1024;
        do {
            _cwd = xrealloc(_cwd, buf_len);
            if (getcwd(_cwd, buf_len) == NULL && errno != ERANGE) {
                LOG_ERRNO("failed to get current working directory");
                goto err;
            }
            buf_len *= 2;
        } while (errno == ERANGE);
        cwd = _cwd;
    }

    /* String lengths, including NULL terminator */
    const size_t cwd_len = strlen(cwd) + 1;
    const size_t override_count = tll_length(overrides);

    const struct client_data data = {
        .hold = hold,
        .no_wait = no_wait,
        .cwd_len = cwd_len,
        .override_count = override_count,
        .argc = argc,
    };

    /* Total packet length, not (yet) including argv[] */
    total_len += sizeof(data) + cwd_len;

    /* Add argv[] size to total packet length */
    cargv = xmalloc(argc * sizeof(cargv[0]));
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
        override_count > (size_t)(unsigned int)data.override_count ||
        argc > (int)(unsigned int)data.argc)
    {
        LOG_ERR("size overflow");
        goto err;
    }

    /* Send everything except argv[] */
    if (send(fd, &(uint32_t){total_len}, sizeof(uint32_t), 0) != sizeof(uint32_t) ||
        send(fd, &data, sizeof(data), 0) != sizeof(data) ||
        send(fd, cwd, cwd_len, 0) != cwd_len)
    {
        LOG_ERRNO("failed to send setup packet to server");
        goto err;
    }

    /* Send overrides */
    tll_foreach(overrides, it) {
        const struct override *o = &it->item;
        struct client_string s = {o->len};
        if (send(fd, &s, sizeof(s), 0) != sizeof(s) ||
            send(fd, o->str, o->len, 0) != o->len)
        {
            LOG_ERRNO("failed to send setup packet (overrides) to server");
            goto err;
        }
    }

    /* Send argv[] */
    for (size_t i = 0; i < argc; i++) {
        if (send(fd, &cargv[i], sizeof(cargv[i]), 0) != sizeof(cargv[i]) ||
            send(fd, argv[i], cargv[i].len, 0) != cargv[i].len)
        {
            LOG_ERRNO("failed to send setup packet (argv) to server");
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

    if (!no_wait || exit_code != 0)
        LOG_INFO("exit-code=%d", exit_code);

    if (rcvd == -1 && errno == EINTR)
        xassert(aborted);
    else if (rcvd != sizeof(exit_code))
        LOG_ERRNO("failed to read server response");
    else
        ret = exit_code;

err:
    tll_foreach(overrides, it) {
        free(it->item.str);
        tll_remove(overrides, it);
    }
    free(cargv);
    free(_cwd);
    if (fd != -1)
        close(fd);
    log_deinit();
    return ret;
}
