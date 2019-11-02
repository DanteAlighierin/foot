#include "fdm.h"

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/epoll.h>

#define LOG_MODULE "fdm"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "tllist.h"

struct handler {
    int fd;
    int events;
    fdm_handler_t callback;
    void *callback_data;
    bool deleted;
};

struct fdm {
    int epoll_fd;
    bool is_polling;
    tll(struct handler *) fds;
    tll(struct handler *) deferred_delete;
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
    *fdm = (struct fdm){
        .epoll_fd = epoll_fd,
        .is_polling = false,
        .fds = tll_init(),
        .deferred_delete = tll_init(),
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

    assert(tll_length(fdm->fds) == 0);
    assert(tll_length(fdm->deferred_delete) == 0);

    tll_free(fdm->fds);
    tll_free(fdm->deferred_delete);
    close(fdm->epoll_fd);
    free(fdm);
}

bool
fdm_add(struct fdm *fdm, int fd, int events, fdm_handler_t handler, void *data)
{
#if defined(_DEBUG)
    tll_foreach(fdm->fds, it) {
        if (it->item->fd == fd) {
            LOG_ERR("FD=%d already registered", fd);
            return false;
        }
    }
#endif

    struct handler *fd_data = malloc(sizeof(*fd_data));
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

bool
fdm_poll(struct fdm *fdm)
{
    assert(!fdm->is_polling && "nested calls to fdm_poll() not allowed");
    if (fdm->is_polling) {
        LOG_ERR("nested calls to fdm_poll() not allowed");
        return false;
    }

    struct epoll_event events[tll_length(fdm->fds)];

    int r = epoll_wait(fdm->epoll_fd, events, tll_length(fdm->fds), -1);
    if (r == -1) {
        if (errno == EINTR)
            return true;

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
