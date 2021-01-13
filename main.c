#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <locale.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <fcntl.h>

#include <fcft/fcft.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "config.h"
#include "foot-features.h"
#include "fdm.h"
#include "reaper.h"
#include "render.h"
#include "server.h"
#include "shm.h"
#include "terminal.h"
#include "util.h"
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
    printf(
        "Usage: %s [OPTIONS...]\n"
        "Usage: %s [OPTIONS...] command [ARGS...]\n"
        "\n"
        "Options:\n"
        "  -c,--config=PATH                      load configuration from PATH ($XDG_CONFIG_HOME/foot/foot.ini)\n"
        "     --check-config                     verify configuration, exit with 0 if ok, otherwise exit with 1\n"
        "  -f,--font=FONT                        comma separated list of fonts in fontconfig format (monospace)\n"
        "  -t,--term=TERM                        value to set the environment variable TERM to (foot)\n"
        "     --title=TITLE                      initial window title (foot)\n"
        "  -a,--app-id=ID                        window application ID (foot)\n"
        "     --maximized                        start in maximized mode\n"
        "     --fullscreen                       start in fullscreen mode\n"
        "     --login-shell                      start shell as a login shell\n"
        "  -w,--window-size-pixels=WIDTHxHEIGHT  initial width and height, in pixels\n"
        "  -W,--window-size-chars=WIDTHxHEIGHT   initial width and height, in characters\n"
        "  -s,--server[=PATH]                    run as a server (use 'footclient' to start terminals).\n"
        "                                        Without PATH, $XDG_RUNTIME_DIR/foot-$WAYLAND_DISPLAY.sock will be used.\n"
        "     --hold                             remain open after child process exits\n"
        "  -p,--print-pid=FILE|FD                print PID to file or FD (only applicable in server mode)\n"
        "  -l,--log-colorize=[never|always|auto] enable/disable colorization of log output on stderr\n"
        "  -s,--log-no-syslog                    disable syslog logging (only applicable in server mode)\n"
        "  -v,--version                          show the version number and quit\n",
        prog_name, prog_name);
}

bool
locale_is_utf8(void)
{
    assert(strlen(u8"ö") == 2);

    wchar_t w;
    if (mbtowc(&w, u8"ö", 2) != 2)
        return false;

    if (w != U'ö')
        return false;

    return true;
}

struct shutdown_context {
    struct terminal **term;
    int exit_code;
};

static void
term_shutdown_cb(void *data, int exit_code)
{
    struct shutdown_context *ctx = data;
    *ctx->term = NULL;
    ctx->exit_code = exit_code;
}

static bool
print_pid(const char *pid_file, bool *unlink_at_exit)
{
    LOG_DBG("printing PID to %s", pid_file);

    errno = 0;
    char *end;
    int pid_fd = strtoul(pid_file, &end, 10);

    if (errno != 0 || *end != '\0') {
        if ((pid_fd = open(pid_file,
                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                           S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
            LOG_ERRNO("%s: failed to open", pid_file);
            return false;
        } else
            *unlink_at_exit = true;
    }

    if (pid_fd >= 0) {
        char pid[32];
        snprintf(pid, sizeof(pid), "%u\n", getpid());

        ssize_t bytes = write(pid_fd, pid, strlen(pid));
        close(pid_fd);

        if (bytes < 0) {
            LOG_ERRNO("failed to write PID to FD=%u", pid_fd);
            return false;
        }

        LOG_DBG("wrote %zd bytes to FD=%d", bytes, pid_fd);
        return true;
    } else
        return false;
}

int
main(int argc, char *const *argv)
{
    int ret = EXIT_FAILURE;

    /* Startup notifications; we don't support it, but must ensure we
     * don't pass this on to programs launched by us */
    unsetenv("DESKTOP_STARTUP_ID");

    const char *const prog_name = argv[0];

    static const struct option longopts[] =  {
        {"config",                 required_argument, NULL, 'c'},
        {"check-config",           no_argument,       NULL, 'C'},
        {"term",                   required_argument, NULL, 't'},
        {"title",                  required_argument, NULL, 'T'},
        {"app-id",                 required_argument, NULL, 'a'},
        {"login-shell",            no_argument,       NULL, 'L'},
        {"font",                   required_argument, NULL, 'f'},
        {"window-size-pixels",     required_argument, NULL, 'w'},
        {"window-size-chars",      required_argument, NULL, 'W'},
        {"server",                 optional_argument, NULL, 's'},
        {"hold",                   no_argument,       NULL, 'H'},
        {"maximized",              no_argument,       NULL, 'm'},
        {"fullscreen",             no_argument,       NULL, 'F'},
        {"presentation-timings",   no_argument,       NULL, 'P'}, /* Undocumented */
        {"print-pid",              required_argument, NULL, 'p'},
        {"log-colorize",           optional_argument, NULL, 'l'},
        {"log-no-syslog",          no_argument,       NULL, 'S'},
        {"version",                no_argument,       NULL, 'v'},
        {"help",                   no_argument,       NULL, 'h'},
        {NULL,                     no_argument,       NULL,   0},
    };

    bool check_config = false;
    const char *conf_path = NULL;
    const char *conf_term = NULL;
    const char *conf_title = NULL;
    const char *conf_app_id = NULL;
    bool login_shell = false;
    tll(char *) conf_fonts = tll_init();
    enum conf_size_type conf_size_type = CONF_SIZE_PX;
    int conf_width = -1;
    int conf_height = -1;
    bool as_server = false;
    const char *conf_server_socket_path = NULL;
    bool presentation_timings = false;
    bool hold = false;
    bool maximized = false;
    bool fullscreen = false;
    bool unlink_pid_file = false;
    const char *pid_file = NULL;
    enum log_colorize log_colorize = LOG_COLORIZE_AUTO;
    bool log_syslog = true;
    user_notifications_t user_notifications = tll_init();

    while (true) {
        int c = getopt_long(argc, argv, "+c:Ct:T:a:Lf:w:W:s::HmFPp:l::Svh", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 'c':
            conf_path = optarg;
            break;

        case 'C':
            check_config = true;
            break;

        case 't':
            conf_term = optarg;
            break;

        case 'L':
            login_shell = true;
            break;

        case 'T':
            conf_title = optarg;
            break;

        case 'a':
            conf_app_id = optarg;
            break;

        case 'f':
            tll_free_and_free(conf_fonts, free);
            for (char *font = strtok(optarg, ","); font != NULL; font = strtok(NULL, ",")) {

                /* Strip leading spaces */
                while (*font != '\0' && isspace(*font))
                    font++;

                /* Strip trailing spaces */
                char *end = font + strlen(font);
                assert(*end == '\0');
                end--;
                while (end > font && isspace(*end))
                    *(end--) = '\0';

                if (strlen(font) == 0)
                    continue;

                tll_push_back(conf_fonts, font);
            }
            break;

        case 'w': {
            unsigned width, height;
            if (sscanf(optarg, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
                fprintf(stderr, "error: invalid window-size-pixels: %s\n", optarg);
                return EXIT_FAILURE;
            }

            conf_size_type = CONF_SIZE_PX;
            conf_width = width;
            conf_height = height;
            break;
        }

        case 'W': {
            unsigned width, height;
            if (sscanf(optarg, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
                fprintf(stderr, "error: invalid window-size-chars: %s\n", optarg);
                return EXIT_FAILURE;
            }

            conf_size_type = CONF_SIZE_CELLS;
            conf_width = width;
            conf_height = height;
            break;
        }

        case 's':
            as_server = true;
            if (optarg != NULL)
                conf_server_socket_path = optarg;
            break;

        case 'P':
            presentation_timings = true;
            break;

        case 'H':
            hold = true;
            break;

        case 'm':
            maximized = true;
            fullscreen = false;
            break;

        case 'F':
            fullscreen = true;
            maximized = false;
            break;

        case 'p':
            pid_file = optarg;
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

        case 'S':
            log_syslog = false;
            break;

        case 'v':
            printf("foot %s\n", version_and_features());
            return EXIT_SUCCESS;

        case 'h':
            print_usage(prog_name);
            return EXIT_SUCCESS;

        case '?':
            return EXIT_FAILURE;
        }
    }

    log_init(log_colorize, as_server && log_syslog,
             as_server ? LOG_FACILITY_DAEMON : LOG_FACILITY_USER, LOG_CLASS_WARNING);

    argc -= optind;
    argv += optind;

    LOG_INFO("%s", version_and_features());

    {
        struct utsname name;
        if (uname(&name) < 0)
            LOG_ERRNO("uname() failed");
        else
            LOG_INFO("arch: %s/%zu-bit", name.machine, sizeof(void *) * 8);
    }

    setlocale(LC_CTYPE, "");
    LOG_INFO("locale: %s", setlocale(LC_CTYPE, NULL));
    if (!locale_is_utf8()) {
        LOG_ERR("locale is not UTF-8");
        return ret;
    }

    struct config conf = {NULL};
    if (!config_load(&conf, conf_path, &user_notifications, check_config)) {
        config_free(conf);
        return ret;
    }

    if (check_config) {
        config_free(conf);
        return EXIT_SUCCESS;
    }

    fcft_set_scaling_filter(conf.tweak.fcft_filter);

    if (conf_term != NULL) {
        free(conf.term);
        conf.term = xstrdup(conf_term);
    }
    if (conf_title != NULL) {
        free(conf.title);
        conf.title = xstrdup(conf_title);
    }
    if (conf_app_id != NULL) {
        free(conf.app_id);
        conf.app_id = xstrdup(conf_app_id);
    }
    if (login_shell)
        conf.login_shell = true;
    if (tll_length(conf_fonts) > 0) {
        for (size_t i = 0; i < ALEN(conf.fonts); i++) {
            tll_foreach(conf.fonts[i], it)
                config_font_destroy(&it->item);
            tll_free(conf.fonts[i]);
        }
        tll_foreach(conf_fonts, it) {
            struct config_font font;
            if (!config_font_parse(it->item, &font)) {
                LOG_ERR("%s: invalid font specification", it->item);
            } else
                tll_push_back(conf.fonts[0], font);
        }
        tll_free(conf_fonts);
    }
    if (conf_width > 0 && conf_height > 0) {
        conf.size.type = conf_size_type;
        conf.size.width = conf_width;
        conf.size.height = conf_height;
    }
    if (conf_server_socket_path != NULL) {
        free(conf.server_socket_path);
        conf.server_socket_path = xstrdup(conf_server_socket_path);
    }
    if (maximized)
        conf.startup_mode = STARTUP_MAXIMIZED;
    else if (fullscreen)
        conf.startup_mode = STARTUP_FULLSCREEN;
    conf.presentation_timings = presentation_timings;
    conf.hold_at_exit = hold;

    struct fdm *fdm = NULL;
    struct reaper *reaper = NULL;
    struct wayland *wayl = NULL;
    struct renderer *renderer = NULL;
    struct terminal *term = NULL;
    struct server *server = NULL;
    struct shutdown_context shutdown_ctx = {.term = &term, .exit_code = EXIT_FAILURE};

    char *cwd = NULL;
    {
        errno = 0;
        size_t buf_len = 1024;
        do {
            cwd = xrealloc(cwd, buf_len);
            if (getcwd(cwd, buf_len) == NULL && errno != ERANGE) {
                LOG_ERRNO("failed to get current working directory");
                goto out;
            }
            buf_len *= 2;
        } while (errno == ERANGE);
    }

    shm_set_max_pool_size(conf.tweak.max_shm_pool_size);

    if ((fdm = fdm_init()) == NULL)
        goto out;

    if ((reaper = reaper_init(fdm)) == NULL)
        goto out;

    if ((wayl = wayl_init(&conf, fdm)) == NULL)
        goto out;

    if ((renderer = render_init(fdm, wayl)) == NULL)
        goto out;

    if (!as_server && (term = term_init(
                           &conf, fdm, reaper, wayl, "foot", cwd, argc, argv,
                           &term_shutdown_cb, &shutdown_ctx)) == NULL) {
        goto out;
    }
    free(cwd);
    cwd = NULL;

    if (as_server && (server = server_init(&conf, fdm, reaper, wayl)) == NULL)
        goto out;

    /* Remember to restore signals in slave */
    const struct sigaction sa = {.sa_handler = &sig_handler};
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        LOG_ERRNO("failed to register signal handlers");
        goto out;
    }

    if (sigaction(SIGHUP, &(struct sigaction){.sa_handler = SIG_IGN}, NULL) < 0) {
        LOG_ERRNO("failed to ignore SIGHUP");
        goto out;
    }

    if (as_server)
        LOG_INFO("running as server; launch terminals by running footclient");

    if (as_server && pid_file != NULL) {
        if (!print_pid(pid_file, &unlink_pid_file))
            goto out;
    }

    while (likely(!aborted && (as_server || tll_length(wayl->terms) > 0))) {
        if (unlikely(!fdm_poll(fdm)))
            break;
    }

    ret = aborted || tll_length(wayl->terms) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

out:
    free(cwd);
    server_destroy(server);
    term_destroy(term);

    shm_fini();
    render_destroy(renderer);
    wayl_destroy(wayl);
    reaper_destroy(reaper);
    fdm_destroy(fdm);

    config_free(conf);

    if (unlink_pid_file)
        unlink(pid_file);

    LOG_INFO("goodbye");
    log_deinit();
    return ret == EXIT_SUCCESS && !as_server ? shutdown_ctx.exit_code : ret;
}
