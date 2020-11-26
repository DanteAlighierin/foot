#include "fdm.h"

#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>

#include <sys/epoll.h>

#include <tllist.h>

#define LOG_MODULE "fdm"
#define LOG_ENABLE_DBG 0
#include "log.h"

struct handler {
    int fd;
    int events;
    fdm_handler_t callback;
    void *callback_data;
    bool deleted;
};

struct hook {
    fdm_hook_t callback;
    void *callback_data;
};

typedef tll(struct hook) hooks_t;

struct fdm {
    int epoll_fd;
    bool is_polling;
    tll(struct handler *) fds;
    tll(struct handler *) deferred_delete;
    hooks_t hooks_low;
    hooks_t hooks_normal;
    hooks_t hooks_high;
};

struct fdm *
fdm_init(void)
{
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        LOG_ERRNO("failed to create epoll FD");
        return NULL;
    }

    struct fdm *fdm = malloc(sizeof(*fdm));
    if (unlikely(fdm == NULL)) {
        LOG_ERRNO("malloc() failed");
        return NULL;
    }

    *fdm = (struct fdm){
        .epoll_fd = epoll_fd,
        .is_polling = false,
        .fds = tll_init(),
        .deferred_delete = tll_init(),
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

    if (tll_length(fdm->hooks_low) > 0 ||
        tll_length(fdm->hooks_normal) > 0 ||
        tll_length(fdm->hooks_high) > 0)
    {
        LOG_WARN("hook list not empty");
    }

    assert(tll_length(fdm->fds) == 0);
    assert(tll_length(fdm->deferred_delete) == 0);
    assert(tll_length(fdm->hooks_low) == 0);
    assert(tll_length(fdm->hooks_normal) == 0);
    assert(tll_length(fdm->hooks_high) == 0);

    tll_free(fdm->fds);
    tll_free(fdm->deferred_delete);
    tll_free(fdm->hooks_low);
    tll_free(fdm->hooks_normal);
    tll_free(fdm->hooks_high);
    close(fdm->epoll_fd);
    free(fdm);
}

bool
fdm_add(struct fdm *fdm, int fd, int events, fdm_handler_t handler, void *data)
{
#if defined(_DEBUG)
    int flags = fcntl(fd, F_GETFL);
    if (!(flags & O_NONBLOCK)) {
        LOG_ERR("FD=%d is in blocking mode", fd);
        assert(false);
        return false;
    }

    tll_foreach(fdm->fds, it) {
        if (it->item->fd == fd) {
            LOG_ERR("FD=%d already registered", fd);
            assert(false);
            return false;
        }
    }
#endif

    struct handler *fd_data = malloc(sizeof(*fd_data));
    if (unlikely(fd_data == NULL)) {
        LOG_ERRNO("malloc() failed");
        return false;
    }

    *fd_data = (struct handler) {
        .fd = fd,
        .events = events,
        .callback = handler,
        .callback_data = data,
        .deleted = false,
    };

    tll_push_back(fdm->fds, fd_data);

    struct epoll_event ev = {
        .events = events,
        .data = {.ptr = fd_data},
    };

    if (epoll_ctl(fdm->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERRNO("failed to register FD=%d with epoll", fd);
        free(fd_data);
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
event_modify(struct fdm *fdm, struct handler *fd, int new_events)
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

    assert(false);
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

bool
fdm_poll(struct fdm *fdm)
{
    assert(!fdm->is_polling && "nested calls to fdm_poll() not allowed");
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

    int r;
    do {
        r = epoll_wait(fdm->epoll_fd, events, tll_length(fdm->fds), -1);
    } while (unlikely(r < 0 && errno == EINTR));

    if (r < 0) {
        LOG_ERRNO("failed to epoll");
        return false;
    }

    bool ret = true;

    fdm->is_polling = true;
    for (int i = 0; i < r; i++) {
        struct handler *fd = events[i].data.ptr;
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
