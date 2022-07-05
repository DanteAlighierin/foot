#include "fdm.h"

#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#include <sys/epoll.h>

#include <tllist.h>

#define LOG_MODULE "fdm"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "xmalloc.h"

struct fd_handler {
    int fd;
    int events;
    fdm_fd_handler_t callback;
    void *callback_data;
    bool deleted;
};

struct sig_handler {
    fdm_signal_handler_t callback;
    void *callback_data;
};

struct hook {
    fdm_hook_t callback;
    void *callback_data;
};

typedef tll(struct hook) hooks_t;

struct fdm {
    int epoll_fd;
    bool is_polling;
    tll(struct fd_handler *) fds;
    tll(struct fd_handler *) deferred_delete;

    sigset_t sigmask;
    struct sig_handler *signal_handlers;

    hooks_t hooks_low;
    hooks_t hooks_normal;
    hooks_t hooks_high;
};

static volatile sig_atomic_t got_signal = false;
static volatile sig_atomic_t *received_signals = NULL;

struct fdm *
fdm_init(void)
{
    sigset_t sigmask;
    if (sigprocmask(0, NULL, &sigmask) < 0) {
        LOG_ERRNO("failed to get process signal mask");
        return NULL;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        LOG_ERRNO("failed to create epoll FD");
        return NULL;
    }

    xassert(received_signals == NULL); /* Only one FDM instance supported */
    received_signals = xcalloc(SIGRTMAX, sizeof(received_signals[0]));
    got_signal = false;

    struct fdm *fdm = malloc(sizeof(*fdm));
    if (unlikely(fdm == NULL)) {
        LOG_ERRNO("malloc() failed");
        return NULL;
    }

    struct sig_handler *sig_handlers = calloc(SIGRTMAX, sizeof(sig_handlers[0]));

    if (sig_handlers == NULL) {
        LOG_ERRNO("failed to allocate signal handler array");
        free(fdm);
        return NULL;
    }

    *fdm = (struct fdm){
        .epoll_fd = epoll_fd,
        .is_polling = false,
        .fds = tll_init(),
        .deferred_delete = tll_init(),
        .sigmask = sigmask,
        .signal_handlers = sig_handlers,
        .hooks_low = tll_init(),
        .hooks_normal = tll_init(),
        .hooks_high = tll_init(),
    };
    return fdm;
}

void
fdm_destroy(struct fdm *fdm)
{
    if (fdm == NULL)
        return;

    if (tll_length(fdm->fds) > 0)
        LOG_WARN("FD list not empty");

    for (int i = 0; i < SIGRTMAX; i++) {
        if (fdm->signal_handlers[i].callback != NULL)
            LOG_WARN("handler for signal %d not removed", i);
    }

    if (tll_length(fdm->hooks_low) > 0 ||
        tll_length(fdm->hooks_normal) > 0 ||
        tll_length(fdm->hooks_high) > 0)
    {
        LOG_WARN("hook list not empty");
    }

    xassert(tll_length(fdm->fds) == 0);
    xassert(tll_length(fdm->deferred_delete) == 0);
    xassert(tll_length(fdm->hooks_low) == 0);
    xassert(tll_length(fdm->hooks_normal) == 0);
    xassert(tll_length(fdm->hooks_high) == 0);

    sigprocmask(SIG_SETMASK, &fdm->sigmask, NULL);
    free(fdm->signal_handlers);

    tll_free(fdm->fds);
    tll_free(fdm->deferred_delete);
    tll_free(fdm->hooks_low);
    tll_free(fdm->hooks_normal);
    tll_free(fdm->hooks_high);
    close(fdm->epoll_fd);
    free(fdm);

    free((void *)received_signals);
    received_signals = NULL;
}

bool
fdm_add(struct fdm *fdm, int fd, int events, fdm_fd_handler_t cb, void *data)
{
#if defined(_DEBUG)
    tll_foreach(fdm->fds, it) {
        if (it->item->fd == fd) {
            BUG("FD=%d already registered", fd);
        }
    }
#endif

    struct fd_handler *handler = malloc(sizeof(*handler));
    if (unlikely(handler == NULL)) {
        LOG_ERRNO("malloc() failed");
        return false;
    }

    *handler = (struct fd_handler) {
        .fd = fd,
        .events = events,
        .callback = cb,
        .callback_data = data,
        .deleted = false,
    };

    tll_push_back(fdm->fds, handler);

    struct epoll_event ev = {
        .events = events,
        .data = {.ptr = handler},
    };

    if (epoll_ctl(fdm->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERRNO("failed to register FD=%d with epoll", fd);
        free(handler);
        tll_pop_back(fdm->fds);
        return false;
    }

    return true;
}

static bool
fdm_del_internal(struct fdm *fdm, int fd, bool close_fd)
{
    if (fd == -1)
        return true;

    tll_foreach(fdm->fds, it) {
        if (it->item->fd != fd)
            continue;

        if (epoll_ctl(fdm->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0)
            LOG_ERRNO("failed to unregister FD=%d from epoll", fd);

        if (close_fd)
            close(it->item->fd);

        it->item->deleted = true;
        if (fdm->is_polling)
            tll_push_back(fdm->deferred_delete, it->item);
        else
            free(it->item);

        tll_remove(fdm->fds, it);
        return true;
    }

    LOG_ERR("no such FD: %d", fd);
    close(fd);
    return false;
}

bool
fdm_del(struct fdm *fdm, int fd)
{
    return fdm_del_internal(fdm, fd, true);
}

bool
fdm_del_no_close(struct fdm *fdm, int fd)
{
    return fdm_del_internal(fdm, fd, false);
}

static bool
event_modify(struct fdm *fdm, struct fd_handler *fd, int new_events)
{
    if (new_events == fd->events)
        return true;

    struct epoll_event ev = {
        .events = new_events,
        .data = {.ptr = fd},
    };

    if (epoll_ctl(fdm->epoll_fd, EPOLL_CTL_MOD, fd->fd, &ev) < 0) {
        LOG_ERRNO("failed to modify FD=%d with epoll (events 0x%08x -> 0x%08x)",
                  fd->fd, fd->events, new_events);
        return false;
    }

    fd->events = new_events;
    return true;
}

bool
fdm_event_add(struct fdm *fdm, int fd, int events)
{
    tll_foreach(fdm->fds, it) {
        if (it->item->fd != fd)
            continue;

        return event_modify(fdm, it->item, it->item->events | events);
    }

    LOG_ERR("FD=%d not registered with the FDM", fd);
    return false;
}

bool
fdm_event_del(struct fdm *fdm, int fd, int events)
{
    tll_foreach(fdm->fds, it) {
        if (it->item->fd != fd)
            continue;

        return event_modify(fdm, it->item, it->item->events & ~events);
    }

    LOG_ERR("FD=%d not registered with the FDM", fd);
    return false;
}

static hooks_t *
hook_priority_to_list(struct fdm *fdm, enum fdm_hook_priority priority)
{
    switch (priority) {
    case FDM_HOOK_PRIORITY_LOW:    return &fdm->hooks_low;
    case FDM_HOOK_PRIORITY_NORMAL: return &fdm->hooks_normal;
    case FDM_HOOK_PRIORITY_HIGH:   return &fdm->hooks_high;
    }

    BUG("unhandled priority type");
    return NULL;
}

bool
fdm_hook_add(struct fdm *fdm, fdm_hook_t hook, void *data,
             enum fdm_hook_priority priority)
{
    hooks_t *hooks = hook_priority_to_list(fdm, priority);

#if defined(_DEBUG)
    tll_foreach(*hooks, it) {
        if (it->item.callback == hook) {
            LOG_ERR("hook=0x%" PRIxPTR " already registered", (uintptr_t)hook);
            return false;
        }
    }
#endif

    tll_push_back(*hooks, ((struct hook){hook, data}));
    return true;
}

bool
fdm_hook_del(struct fdm *fdm, fdm_hook_t hook, enum fdm_hook_priority priority)
{
    hooks_t *hooks = hook_priority_to_list(fdm, priority);

    tll_foreach(*hooks, it) {
        if (it->item.callback != hook)
            continue;

        tll_remove(*hooks, it);
        return true;
    }

    LOG_WARN("hook=0x%" PRIxPTR " not registered", (uintptr_t)hook);
    return false;
}

static void
signal_handler(int signo)
{
    got_signal = true;
    received_signals[signo] = true;
}

bool
fdm_signal_add(struct fdm *fdm, int signo, fdm_signal_handler_t handler, void *data)
{
    if (fdm->signal_handlers[signo].callback != NULL) {
        LOG_ERR("signal %d already has a handler", signo);
        return false;
    }

    sigset_t mask, original;
    sigemptyset(&mask);
    sigaddset(&mask, signo);

    if (sigprocmask(SIG_BLOCK, &mask, &original) < 0) {
        LOG_ERRNO("failed to block signal %d", signo);
        return false;
    }

    struct sigaction action = {.sa_handler = &signal_handler};
    sigemptyset(&action.sa_mask);
    if (sigaction(signo, &action, NULL) < 0) {
        LOG_ERRNO("failed to set signal handler for signal %d", signo);
        sigprocmask(SIG_SETMASK, &original, NULL);
        return false;
    }

    received_signals[signo] = false;
    fdm->signal_handlers[signo].callback = handler;
    fdm->signal_handlers[signo].callback_data = data;
    return true;
}

bool
fdm_signal_del(struct fdm *fdm, int signo)
{
    if (fdm->signal_handlers[signo].callback == NULL)
        return false;

    struct sigaction action = {.sa_handler = SIG_DFL};
    sigemptyset(&action.sa_mask);
    if (sigaction(signo, &action, NULL) < 0) {
        LOG_ERRNO("failed to restore signal handler for signal %d", signo);
        return false;
    }

    received_signals[signo] = false;
    fdm->signal_handlers[signo].callback = NULL;
    fdm->signal_handlers[signo].callback_data = NULL;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signo);
    if (sigprocmask(SIG_UNBLOCK, &mask, NULL) < 0) {
        LOG_ERRNO("failed to unblock signal %d", signo);
        return false;
    }

    return true;
}

bool
fdm_poll(struct fdm *fdm)
{
    xassert(!fdm->is_polling && "nested calls to fdm_poll() not allowed");
    if (fdm->is_polling) {
        LOG_ERR("nested calls to fdm_poll() not allowed");
        return false;
    }

    tll_foreach(fdm->hooks_high, it) {
        LOG_DBG(
            "executing high priority hook 0x%" PRIxPTR" (fdm=%p, data=%p)",
            (uintptr_t)it->item.callback, (void *)fdm,
            (void *)it->item.callback_data);
        it->item.callback(fdm, it->item.callback_data);
    }
    tll_foreach(fdm->hooks_normal, it) {
        LOG_DBG(
            "executing normal priority hook 0x%" PRIxPTR " (fdm=%p, data=%p)",
            (uintptr_t)it->item.callback, (void *)fdm,
            (void *)it->item.callback_data);
        it->item.callback(fdm, it->item.callback_data);
    }
    tll_foreach(fdm->hooks_low, it) {
        LOG_DBG(
            "executing low priority hook 0x%" PRIxPTR " (fdm=%p, data=%p)",
            (uintptr_t)it->item.callback, (void *)fdm,
            (void *)it->item.callback_data);
        it->item.callback(fdm, it->item.callback_data);
    }

    struct epoll_event events[tll_length(fdm->fds)];

    int r = epoll_pwait(
        fdm->epoll_fd, events, tll_length(fdm->fds), -1, &fdm->sigmask);

    int errno_copy = errno;

    if (unlikely(got_signal)) {
        got_signal = false;

        for (int i = 0; i < SIGRTMAX; i++) {
            if (received_signals[i]) {
                received_signals[i] = false;
                struct sig_handler *handler = &fdm->signal_handlers[i];

                xassert(handler->callback != NULL);
                if (!handler->callback(fdm, i, handler->callback_data))
                    return false;
            }
        }
    }

    if (unlikely(r < 0)) {
        if (errno_copy == EINTR)
            return true;

        LOG_ERRNO_P(errno_copy, "failed to epoll");
        return false;
    }

    bool ret = true;

    fdm->is_polling = true;
    for (int i = 0; i < r; i++) {
        struct fd_handler *fd = events[i].data.ptr;
        if (fd->deleted)
            continue;

        if (!fd->callback(fdm, fd->fd, events[i].events, fd->callback_data)) {
            ret = false;
            break;
        }
    }
    fdm->is_polling = false;

    tll_foreach(fdm->deferred_delete, it) {
        free(it->item);
        tll_remove(fdm->deferred_delete, it);
    }

    return ret;
}
