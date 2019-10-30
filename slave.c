#define _XOPEN_SOURCE 500
#include "slave.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "slave"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "tokenize.h"

static void
slave_exec(int ptmx, char *const argv[], int err_fd)
{
    int pts = -1;
    const char *pts_name = ptsname(ptmx);

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
    if (pts != -1)
        close(pts);
    if (ptmx != -1)
        close(ptmx);
    _exit(errno);
}

pid_t
slave_spawn(int ptmx, int argc, char *const *argv,
            const char *conf_shell)
{
    int fork_pipe[2];
    if (pipe2(fork_pipe, O_CLOEXEC) < 0) {
        LOG_ERRNO("failed to create pipe");
        return -1;
    }

    pid_t pid = fork();
    switch (pid) {
    case -1:
        LOG_ERRNO("failed to fork");
        close(fork_pipe[0]);
        close(fork_pipe[1]);
        return -1;

    case 0:
        /* Child */
        close(fork_pipe[0]);  /* Close read end */

        char **_shell_argv = NULL;
        char *const *shell_argv = argv;

        if (argc == 0) {
            char *shell_copy = strdup(conf_shell);
            if (!tokenize_cmdline(shell_copy, &_shell_argv)) {
                free(shell_copy);
                (void)!write(fork_pipe[1], &errno, sizeof(errno));
                _exit(0);
            }
            shell_argv = _shell_argv;
        }

        slave_exec(ptmx, shell_argv, fork_pipe[1]);
        assert(false);
        break;

    default: {
        close(fork_pipe[1]); /* Close write end */
        LOG_DBG("slave has PID %d", pid);

        int _errno;
        static_assert(sizeof(errno) == sizeof(_errno), "errno size mismatch");

        ssize_t ret = read(fork_pipe[0], &_errno, sizeof(_errno));
        close(fork_pipe[0]);

        if (ret < 0) {
            LOG_ERRNO("failed to read from pipe");
            return -1;
        } else if (ret == sizeof(_errno)) {
            LOG_ERRNO_P(
                "%s: failed to execute", _errno, argc == 0 ? conf_shell : argv[0]);
            return -1;
        } else
            LOG_DBG("%s: successfully started", conf_shell);
        break;
    }
    }

    return pid;
}
