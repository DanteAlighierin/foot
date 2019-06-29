#define _XOPEN_SOURCE 500
#include "slave.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "slave"
#define LOG_ENABLE_DBG 1
#include "log.h"

void
slave_spawn(int ptmx)
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

    execl("/usr/bin/zsh", "/usr/bin/zsh", NULL);

err:
    if (pts != -1)
        close(pts);
    if (ptmx != -1)
        close(ptmx);
    _exit(errno);
}
