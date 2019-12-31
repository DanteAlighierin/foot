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

#include <sys/sysinfo.h>

#include <fcft/fcft.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "config.h"
#include "fdm.h"
#include "server.h"
#include "shm.h"
#include "terminal.h"
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
    printf(
        "Usage: %s [OPTIONS]...\n"
        "Usage: %s [OPTIONS]... -- command\n"
        "\n"
        "Options:\n"
        "  -c,--config=PATH            load configuration from PATH (XDG_CONFIG_HOME/footrc)\n"
        "  -f,--font=FONT              comma separated list of fonts in fontconfig format (monospace)\n"
        "  -t,--term=TERM              value to set the environment variable TERM to (foot)\n"
        "  -g,--geometry=WIDTHxHEIGHT  set initial width and height\n"
        "  -s,--server[=PATH]          run as a server (use 'footclient' to start terminals).\n"
        "                              Without PATH, XDG_RUNTIME_DIR/foot.sock will be used.\n"
        "  -v,--version                show the version number and quit\n",
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

int
main(int argc, char *const *argv)
{
    int ret = EXIT_FAILURE;

    /* Startup notifications; we don't support it, but must ensure we
     * don't pass this on to programs launched by us */
    unsetenv("DESKTOP_STARTUP_ID");

    const char *const prog_name = argv[0];

    static const struct option longopts[] =  {
        {"config",               required_argument, NULL, 'c'},
        {"term",                 required_argument, NULL, 't'},
        {"font",                 required_argument, NULL, 'f'},
        {"geometry",             required_argument, NULL, 'g'},
        {"server",               optional_argument, NULL, 's'},
        {"presentation-timings", no_argument,       NULL, 'p'}, /* Undocumented */
        {"version",              no_argument,       NULL, 'v'},
        {"help",                 no_argument,       NULL, 'h'},
        {NULL,                   no_argument,       NULL,   0},
    };

    const char *conf_path = NULL;
    const char *conf_term = NULL;
    tll(char *) conf_fonts = tll_init();
    int conf_width = -1;
    int conf_height = -1;
    bool as_server = false;
    const char *conf_server_socket_path = NULL;
    bool presentation_timings = false;

    while (true) {
        int c = getopt_long(argc, argv, "c:tf:g:s::pvh", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 'c':
            conf_path = optarg;
            break;

        case 't':
            conf_term = optarg;
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

                tll_push_back(conf_fonts, strdup(font));
            }
            break;

        case 'g': {
            unsigned width, height;
            if (sscanf(optarg, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
                fprintf(stderr, "error: invalid geometry: %s\n", optarg);
                return EXIT_FAILURE;
            }

            conf_width = width;
            conf_height = height;
            break;
        }

        case 's':
            as_server = true;
            if (optarg != NULL)
                conf_server_socket_path = optarg;
            break;

        case 'p':
            presentation_timings = true;
            break;

        case 'v':
            printf("foot version %s\n", FOOT_VERSION);
            return EXIT_SUCCESS;

        case 'h':
            print_usage(prog_name);
            return EXIT_SUCCESS;

        case '?':
            return EXIT_FAILURE;
        }
    }

    log_init(as_server ? LOG_FACILITY_DAEMON : LOG_FACILITY_USER, LOG_CLASS_WARNING);

    argc -= optind;
    argv += optind;

    struct config conf = {NULL};
    if (!config_load(&conf, conf_path))
        return ret;

    setlocale(LC_ALL, "");
    if (!locale_is_utf8()) {
        LOG_ERR("locale is not UTF-8");
        return ret;
    }

    if (conf_term != NULL) {
        free(conf.term);
        conf.term = strdup(conf_term);
    }
    if (tll_length(conf_fonts) > 0) {
        tll_free_and_free(conf.fonts, free);
        tll_foreach(conf_fonts, it)
            tll_push_back(conf.fonts, it->item);
        tll_free(conf_fonts);
    }
    if (conf_width > 0)
        conf.width = conf_width;
    if (conf_height > 0)
        conf.height = conf_height;
    if (conf_server_socket_path != NULL) {
        free(conf.server_socket_path);
        conf.server_socket_path = strdup(conf_server_socket_path);
    }
    conf.presentation_timings = presentation_timings;

    struct fdm *fdm = NULL;
    struct wayland *wayl = NULL;
    struct terminal *term = NULL;
    struct server *server = NULL;
    struct shutdown_context shutdown_ctx = {.term = &term, .exit_code = EXIT_FAILURE};

    char *cwd = NULL;
    {
        size_t buf_len = 1024;
        do {
            cwd = realloc(cwd, buf_len);
            getcwd(cwd, buf_len);
            buf_len *= 2;
        } while (errno == ERANGE);
    }

    if ((fdm = fdm_init()) == NULL)
        goto out;

    if ((wayl = wayl_init(&conf, fdm)) == NULL)
        goto out;

    if (!as_server && (term = term_init(
                           &conf, fdm, wayl, conf.term, "foot", cwd, argc, argv,
                           &term_shutdown_cb, &shutdown_ctx)) == NULL) {
        free(cwd);
        goto out;
    }
    free(cwd);

    if (as_server && (server = server_init(&conf, fdm, wayl)) == NULL)
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

    while (!aborted && (as_server || tll_length(wayl->terms) > 0)) {
        if (!fdm_poll(fdm))
            break;
    }

    ret = aborted || tll_length(wayl->terms) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

out:
    server_destroy(server);
    term_destroy(term);

    shm_fini();
    wayl_destroy(wayl);
    fdm_destroy(fdm);

    config_free(conf);

    LOG_INFO("goodbye");
    log_deinit();
    return ret == EXIT_SUCCESS && !as_server ? shutdown_ctx.exit_code : ret;
}
