#include "spawn.h"

#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "spawn"
#define LOG_ENABLE_DBG 0
#include "log.h"

bool
spawn(struct reaper *reaper, const char *cwd, char *const argv[],
      int stdin_fd, int stdout_fd, int stderr_fd)
{
    int pipe_fds[2] = {-1, -1};
    if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
        LOG_ERRNO("failed to create pipe");
        goto err;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERRNO("failed to fork");
        goto err;
    }

    if (pid == 0) {
        /* Child */
        close(pipe_fds[0]);

        if ((stdin_fd >= 0 && (dup2(stdin_fd, STDIN_FILENO) < 0 || close(stdin_fd) < 0)) ||
            (stdout_fd >= 0 && (dup2(stdout_fd, STDOUT_FILENO) < 0 || close(stdout_fd) < 0)) ||
            (stderr_fd >= 0 && (dup2(stderr_fd, STDERR_FILENO) < 0 || close(stderr_fd) < 0)) ||
            (cwd != NULL && chdir(cwd) < 0) ||
            execvp(argv[0], argv) < 0)
        {
            (void)!write(pipe_fds[1], &errno, sizeof(errno));
            _exit(errno);
        }
        assert(false);
        _exit(errno);
    }

    /* Parent */
    close(pipe_fds[1]);

    int _errno;
    static_assert(sizeof(_errno) == sizeof(errno), "errno size mismatch");

    ssize_t ret = read(pipe_fds[0], &_errno, sizeof(_errno));
    close(pipe_fds[0]);

    if (ret == 0) {
        reaper_add(reaper, pid);
        return true;
    } else if (ret < 0) {
        LOG_ERRNO("failed to read from pipe");
        return false;
    } else {
        LOG_ERRNO_P("%s: failed to spawn", _errno, argv[0]);
        errno = _errno;
        waitpid(pid, NULL, 0);
        return false;
    }

err:
    if (pipe_fds[0] != -1)
        close(pipe_fds[0]);
    if (pipe_fds[1] != -1)
        close(pipe_fds[1]);
    return false;
}
