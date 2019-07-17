#define _XOPEN_SOURCE 500
#include "slave.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "slave"
#define LOG_ENABLE_DBG 0
#include "log.h"

static bool
push_argv(char ***argv, size_t *size, char *arg, size_t *argc)
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

    (*argv)[(*argc)++] = arg;
    return true;
}

static bool
tokenize_cmdline(char *cmdline, char ***argv)
{
    *argv = NULL;
    size_t argv_size = 0;

    bool first_token_is_quoted = cmdline[0] == '"' || cmdline[0] == '\'';
    char delim = first_token_is_quoted ? cmdline[0] : ' ';

    char *p = first_token_is_quoted ? &cmdline[1] : &cmdline[0];

    size_t idx = 0;
    while (*p != '\0') {
        char *end = strchr(p, delim);
        if (end == NULL) {
            if (delim != ' ') {
                LOG_ERR("unterminated %s quote\n", delim == '"' ? "double" : "single");
                free(*argv);
                return false;
            }

            if (!push_argv(argv, &argv_size, p, &idx) ||
                !push_argv(argv, &argv_size, NULL, &idx))
            {
                goto err;
            } else
                return true;
        }

        *end = '\0';

        if (!push_argv(argv, &argv_size, p, &idx))
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
    }

    if (!push_argv(argv, &argv_size, NULL, &idx))
        goto err;

    return true;

err:
    free(*argv);
    return false;
}

void
slave_spawn(int ptmx, char *cmd, int err_fd)
{
    int pts = -1;
    const char *pts_name = ptsname(ptmx);

    char **argv = NULL;
    if (!tokenize_cmdline(cmd, &argv))
        goto err;

    if (grantpt(ptmx) == -1) {
        LOG_ERRNO("failed to grantpt()");
        goto err;
    }
    if (unlockpt(ptmx) == -1) {
        LOG_ERRNO("failed to unlockpt()");
        goto err;
    }

    close(ptmx);
    ptmx = -1;

    if (setsid() == -1) {
        LOG_ERRNO("failed to setsid()");
        goto err;
    }

    pts = open(pts_name, O_RDWR);
    if (pts == -1) {
        LOG_ERRNO("failed to open pseudo terminal slave device");
        goto err;
    }

    if (dup2(pts, STDIN_FILENO) == -1 ||
        dup2(pts, STDOUT_FILENO) == -1 ||
        dup2(pts, STDERR_FILENO) == -1)
    {
        LOG_ERRNO("failed to dup stdin/stdout/stderr");
        goto err;
    }

    close(pts);
    pts = -1;

    execvp(argv[0], argv);

err:
    (void)!write(err_fd, &errno, sizeof(errno));
    if (argv)
        free(argv);
    if (pts != -1)
        close(pts);
    if (ptmx != -1)
        close(ptmx);
    _exit(errno);
}
