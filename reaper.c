#include "reaper.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <tllist.h>

#define LOG_MODULE "reaper"
#define LOG_ENABLE_DBG 0
#include "log.h"

struct child {
    pid_t pid;
    reaper_cb cb;
    void *cb_data;
};

struct reaper {
    struct fdm *fdm;
    tll(struct child) children;
};

static bool fdm_reap(struct fdm *fdm, int signo, void *data);

struct reaper *
reaper_init(struct fdm *fdm)
{
    struct reaper *reaper = malloc(sizeof(*reaper));
    if (unlikely(reaper == NULL)) {
        LOG_ERRNO("malloc() failed");
        return NULL;
    }

    *reaper = (struct reaper){
        .fdm = fdm,
        .children = tll_init(),
    };

    if (!fdm_signal_add(fdm, SIGCHLD, &fdm_reap, reaper))
        goto err;

    return reaper;

err:
    tll_free(reaper->children);
    free(reaper);
    return NULL;
}

void
reaper_destroy(struct reaper *reaper)
{
    if (reaper == NULL)
        return;

    fdm_signal_del(reaper->fdm, SIGCHLD);
    tll_free(reaper->children);
    free(reaper);
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
fdm_reap(struct fdm *fdm, int signo, void *data)
{
    struct reaper *reaper = data;

    while (true) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;

        if (WIFEXITED(status))
            LOG_DBG("pid=%d: exited with status=%d", pid, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            LOG_DBG("pid=%d: killed by signal=%d", pid, WTERMSIG(status));
        else
            LOG_DBG("pid=%d: died of unknown resason", pid);

        tll_foreach(reaper->children, it) {
            struct child *_child = &it->item;

            if (_child->pid != pid)
                continue;

            /* Make sure we remove it *before* the callback, since it too
             * may remove it */
            struct child child = it->item;
            tll_remove(reaper->children, it);

            if (child.cb != NULL)
                child.cb(reaper, child.pid, status, child.cb_data);

            break;
        }
    }

    return true;
}
