#include "async.h"

#include <stdint.h>
#include <errno.h>
#include <unistd.h>

#define LOG_MODULE "async"
#define LOG_ENABLE_DBG 0
#include "log.h"

enum async_write_status
async_write(int fd, const void *_data, size_t len, size_t *idx)
{
    const uint8_t *const data = _data;
    size_t left = len - *idx;

    while (left > 0) {
        ssize_t ret = write(fd, &data[*idx], left);

        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return ASYNC_WRITE_REMAIN;

            return ASYNC_WRITE_ERR;
        }

        LOG_DBG("wrote %zd bytes of %zu (%zu left) to FD=%d",
                ret, left, left - ret, fd);

        *idx += ret;
        left -= ret;
    }

    return ASYNC_WRITE_DONE;
}
