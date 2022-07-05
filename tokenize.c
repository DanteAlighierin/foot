#include "tokenize.h"

#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "tokenize"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "xmalloc.h"

static bool
push_argv(char ***argv, size_t *size, const char *arg, size_t len, size_t *argc)
{
    if (arg != NULL && arg[0] == '%')
        return true;

    if (*argc >= *size) {
        size_t new_size = *size > 0 ? 2 * *size : 10;
        char **new_argv = realloc(*argv, new_size * sizeof(new_argv[0]));

        if (new_argv == NULL)
            return false;

        *argv = new_argv;
        *size = new_size;
    }

    (*argv)[(*argc)++] = arg != NULL ? xstrndup(arg, len) : NULL;
    return true;
}

bool
tokenize_cmdline(const char *cmdline, char ***argv)
{
    *argv = NULL;
    size_t argv_size = 0;

    const char *final_end = cmdline + strlen(cmdline) + 1;

    bool first_token_is_quoted = cmdline[0] == '"' || cmdline[0] == '\'';
    char delim = first_token_is_quoted ? cmdline[0] : ' ';

    const char *p = first_token_is_quoted ? &cmdline[1] : &cmdline[0];
    const char *search_start = p;

    size_t idx = 0;
    while (*p != '\0') {
        char *end = strchr(search_start, delim);
        if (end == NULL) {
            if (delim != ' ') {
                LOG_ERR("unterminated %s quote", delim == '"' ? "double" : "single");
                goto err;
            }

            if (!push_argv(argv, &argv_size, p, final_end - p, &idx) ||
                !push_argv(argv, &argv_size, NULL, 0, &idx))
            {
                goto err;
            } else
                return true;
        }

        if (end > p && *(end - 1) == '\\') {
            /* Escaped quote, remove one level of escaping and
             * continue searching for "our" closing quote */
            memmove(end - 1, end, strlen(end));
            end[strlen(end) - 1] = '\0';
            search_start = end;
            continue;
        }

        //*end = '\0';

        if (!push_argv(argv, &argv_size, p, end - p, &idx))
            goto err;

        p = end + 1;
        while (*p == delim)
            p++;

        while (*p == ' ')
            p++;

        if (*p == '"' || *p == '\'') {
            delim = *p;
            p++;
        } else
            delim = ' ';
        search_start = p;
    }

    if (!push_argv(argv, &argv_size, NULL, 0, &idx))
        goto err;

    return true;

err:
    for (size_t i = 0; i < idx; i++)
        free((*argv)[i]);
    free(*argv);
    *argv = NULL;
    return false;
}
