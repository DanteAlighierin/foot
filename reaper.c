#include "reaper.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <tllist.h>

#define LOG_MODULE "reaper"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"

struct child {
    pid_t pid;
    reaper_cb cb;
    void *cb_data;
};

struct reaper {
    struct fdm *fdm;
    int fd;
    tll(struct child) children;
};

static bool fdm_reap(struct fdm *fdm, int fd, int events, void *data);

struct reaper *
reaper_init(struct fdm *fdm)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

    /* Block normal signal handling - we're using a signalfd instead */
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        LOG_ERRNO("failed to block SIGCHLD");
        return NULL;
    }

    int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) {
        LOG_ERRNO("failed to create signal FD");
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
        return NULL;
    }

    struct reaper *reaper = malloc(sizeof(*reaper));
    if (unlikely(reaper == NULL)) {
        LOG_ERRNO("malloc() failed");
        close(fd);
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
        return NULL;
    }

    *reaper = (struct reaper){
        .fdm = fdm,
        .fd = fd,
        .children = tll_init(),
    };

    if (!fdm_add(fdm, fd, EPOLLIN, &fdm_reap, reaper)){
        LOG_ERR("failed to register with the FDM");
        goto err;
    }

    return reaper;

err:
    tll_free(reaper->children);
    close(fd);
    free(reaper);
    return NULL;
}

void
reaper_destroy(struct reaper *reaper)
{
    if (reaper == NULL)
        return;

    fdm_del(reaper->fdm, reaper->fd);
    tll_free(reaper->children);
    free(reaper);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

void
reaper_add(struct reaper *reaper, pid_t pid, reaper_cb cb, void *cb_data)
{
    LOG_DBG("adding pid=%d", pid);
    tll_push_back(
        reaper->children,
        ((struct child){.pid = pid, .cb = cb, .cb_data = cb_data}));
}

void
reaper_del(struct reaper *reaper, pid_t pid)
{
    tll_foreach(reaper->children, it) {
        if (it->item.pid == pid) {
            tll_remove(reaper->children, it);
            break;
        }
    }
}

static bool
fdm_reap(struct fdm *fdm, int fd, int events, void *data)
{
    struct reaper *reaper = data;

    bool pollin = events & EPOLLIN;
    bool hup = events & EPOLLHUP;

    if (hup && !pollin)
        return false;

    assert(pollin);

    struct signalfd_siginfo info;
    ssize_t amount = read(reaper->fd, &info, sizeof(info));

    if (amount < 0) {
        LOG_ERRNO("failed to read");
        return false;
    }

    assert((size_t)amount >= sizeof(info));

    if (info.ssi_signo != SIGCHLD) {
        LOG_WARN("got non-SIGCHLD signal: %d", info.ssi_signo);
        return true;
    }

    tll_foreach(reaper->children, it) {
        struct child *_child = &it->item;

        if (_child->pid != (pid_t)info.ssi_pid)
            continue;

        /* Make sure we remove it *before* the callback, since it too
         * may remove it */
        struct child child = it->item;
        tll_remove(reaper->children, it);

        bool reap_ourselves = true;
        if (child.cb != NULL)
            reap_ourselves = !child.cb(reaper, child.pid, child.cb_data);

        if (reap_ourselves) {
            int result;
            int res = waitpid(child.pid, &result, WNOHANG);

            if (res <= 0) {
                if (res < 0)
                    LOG_ERRNO("waitpid failed for pid=%d", child.pid);
                continue;
            }

            else if (WIFEXITED(result))
                LOG_DBG("pid=%d: exited with status=%d", pid, WEXITSTATUS(result));
            else if (WIFSIGNALED(result))
                LOG_DBG("pid=%d: killed by signal=%d", pid, WTERMSIG(result));
            else
                LOG_DBG("pid=%d: died of unknown resason", pid);
        }
    }

    if (hup)
        return false;

    return true;
}
