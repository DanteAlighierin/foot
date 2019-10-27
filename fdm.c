#include "fdm.h"

#include <stdlib.h>
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
};

struct fdm {
    int epoll_fd;
    tll(struct fd) fds;
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
        .fds = tll_init(),
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
        if (it->item.fd == fd) {
            LOG_ERR("FD=%d already registered", fd);
            return false;
        }
    }
#endif

    tll_push_back(
        fdm->fds, ((struct fd){.fd = fd, .data = data, .handler = handler}));

    struct epoll_event ev = {
        .events = events,
        .data = {.ptr = &tll_back(fdm->fds)},
    };

    if (epoll_ctl(fdm->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERRNO("failed to register FD with epoll");
        tll_pop_back(fdm->fds);
        return false;
    }

    return true;
}

bool
fdm_del(struct fdm *fdm, int fd)
{
    tll_foreach(fdm->fds, it) {
        if (it->item.fd == fd) {
            if (epoll_ctl(fdm->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0)
                LOG_ERRNO("failed to unregister FD=%d from epoll", fd);

            tll_remove(fdm->fds, it);
            return true;
        }
    }

    LOG_ERR("no such FD: %d", fd);
    return false;
}

bool
fdm_poll(struct fdm *fdm)
{
    struct epoll_event events[tll_length(fdm->fds)];
    int ret = epoll_wait(fdm->epoll_fd, events, tll_length(fdm->fds), -1);
    if (ret == -1) {
        if (errno == EINTR)
            return true;

        LOG_ERRNO("failed to epoll");
        return false;
    }

    for (int i = 0; i < ret; i++) {
        struct fd *fd = events[i].data.ptr;
        if (!fd->handler(fdm, fd->fd, events[i].events, fd->data))
            return false;
    }

    return true;
}
