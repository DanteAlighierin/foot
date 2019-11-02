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

struct fd {
    int fd;
    void *data;
    fdm_handler_t handler;
    bool deleted;
};

struct fdm {
    int epoll_fd;
    bool is_polling;
    tll(struct fd *) fds;
    tll(struct fd *) deferred_delete;
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

    tll_free(fdm->fds);
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

    struct fd *fd_data = malloc(sizeof(*fd_data));
    *fd_data = (struct fd) {
        .fd = fd,
        .data = data,
        .handler = handler,
        .deleted = false,
    };

    tll_push_back(fdm->fds, fd_data);

    struct epoll_event ev = {
        .events = events,
        .data = {.ptr = fd_data},
    };

    if (epoll_ctl(fdm->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERRNO("failed to register FD with epoll");
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
        if (it->item->fd == fd) {
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

bool
fdm_poll(struct fdm *fdm)
{
    assert(!fdm->is_polling && "nested calls to fdm_poll() not allowed");
    if (fdm->is_polling) {
        LOG_ERR("nested calls to fdm_poll() not allowed");
        return false;
    }

    struct epoll_event events[tll_length(fdm->fds)];
    int ret = epoll_wait(fdm->epoll_fd, events, tll_length(fdm->fds), -1);
    if (ret == -1) {
        if (errno == EINTR)
            return true;

        LOG_ERRNO("failed to epoll");
        return false;
    }

    fdm->is_polling = true;
    for (int i = 0; i < ret; i++) {
        struct fd *fd = events[i].data.ptr;
        if (fd->deleted)
            continue;

        if (!fd->handler(fdm, fd->fd, events[i].events, fd->data)) {
            fdm->is_polling = false;
            return false;
        }
    }
    fdm->is_polling = false;

    tll_foreach(fdm->deferred_delete, it) {
        free(it->item);
        tll_remove(fdm->deferred_delete, it);
    }

    return true;
}
