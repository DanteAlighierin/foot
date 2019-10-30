#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <locale.h>
#include <getopt.h>
#include <errno.h>

#include <sys/sysinfo.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "config.h"
#include "fdm.h"
#include "font.h"
#include "shm.h"
#include "terminal.h"
#include "version.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

static void
print_usage(const char *prog_name)
{
    printf("Usage: %s [OPTION]...\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -f,--font=FONT              comma separated list of fonts in fontconfig format (monospace)\n"
           "  -t,--term=TERM              value to set the environment variable TERM to (foot)\n"
           "  -g,--geometry=WIDTHxHEIGHT  set initial width and height\n"
           "  -v,--version                show the version number and quit\n");
    printf("\n");
}

int
main(int argc, char *const *argv)
{
    int ret = EXIT_FAILURE;

    /* Startup notifications; we don't support it, but must ensure we
     * don't pass this on to programs launched by us */
    unsetenv("DESKTOP_STARTUP_ID");

    struct config conf = {NULL};
    if (!config_load(&conf))
        return ret;

    const char *const prog_name = argv[0];

    static const struct option longopts[] =  {
        {"term",     required_argument, 0, 't'},
        {"font",     required_argument, 0, 'f'},
        {"geometry", required_argument, 0, 'g'},
        {"version",  no_argument,       0, 'v'},
        {"help",     no_argument,       0, 'h'},
        {NULL,       no_argument,       0,   0},
    };

    while (true) {
        int c = getopt_long(argc, argv, ":t:f:g:vh", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 't':
            free(conf.term);
            conf.term = strdup(optarg);
            break;

        case 'f':
            tll_free_and_free(conf.fonts, free);
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

                tll_push_back(conf.fonts, strdup(font));
            }
            break;

        case 'g': {
            unsigned width, height;
            if (sscanf(optarg, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
                fprintf(stderr, "error: invalid geometry: %s\n", optarg);
                return EXIT_FAILURE;
            }

            conf.width = width;
            conf.height = height;
            break;
        }

        case 'v':
            printf("foot version %s\n", FOOT_VERSION);
            config_free(conf);
            return EXIT_SUCCESS;

        case 'h':
            print_usage(prog_name);
            config_free(conf);
            return EXIT_SUCCESS;

        case ':':
            fprintf(stderr, "error: -%c: missing required argument\n", optopt);
            config_free(conf);
            return EXIT_FAILURE;

        case '?':
            fprintf(stderr, "error: -%c: invalid option\n", optopt);
            config_free(conf);
            return EXIT_FAILURE;
        }
    }

    argc -= optind;
    argv += optind;

    setlocale(LC_ALL, "");
    setenv("TERM", conf.term, 1);

    struct fdm *fdm = NULL;
    struct wayland *wayl = NULL;
    struct terminal *term = NULL;

    if ((fdm = fdm_init()) == NULL)
        goto out;

    if ((wayl = wayl_init(fdm)) == NULL)
        goto out;

    if ((term = term_init(&conf, fdm, wayl, argc, argv)) == NULL)
        goto out;

    while (tll_length(wayl->terms) > 0) {
        wl_display_flush(wayl->display);  /* TODO: figure out how to get rid of this */

        if (!fdm_poll(fdm))
            break;
    }

    ret = tll_length(wayl->terms) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

out:
    shm_fini();

    tll_foreach(wayl->terms, it)
        term_destroy(it->item);

    int child_ret = wayl->last_exit_value;

    wayl_destroy(wayl);
    fdm_destroy(fdm);
    config_free(conf);

    return ret == EXIT_SUCCESS ? child_ret : ret;

}
