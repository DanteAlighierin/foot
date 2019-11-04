#pragma once

#include <stddef.h>

enum async_write_status {ASYNC_WRITE_DONE, ASYNC_WRITE_REMAIN, ASYNC_WRITE_ERR};

/*
 * Primitive that writes data to a NONBLOCK:ing FD.
 *
 * _data: points to the beginning of the buffer
 * len: total size of the data buffer
 * idx: pointer to byte offset into data buffer - writing starts here.
 *
 * Thus, the total amount of data to write is (len - *idx). *idx is
 * updated such that it points to the next unwritten byte in the data
 * buffer.
 *
 * I.e. if the return value is:
 *  - ASYNC_WRITE_DONE, then the *idx == len.
 *  - ASYNC_WRITE_REMAIN, then *idx < len
 *  - ASYNC_WRITE_ERR, there was an error, and no data was written
 */
enum async_write_status async_write(
    int fd, const void *data, size_t len, size_t *idx);
