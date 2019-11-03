#pragma once

#include <stdbool.h>

struct fdm;

typedef bool (*fdm_handler_t)(struct fdm *fdm, int fd, int events, void *data);

struct fdm *fdm_init(void);
void fdm_destroy(struct fdm *fdm);

bool fdm_add(struct fdm *fdm, int fd, int events, fdm_handler_t handler, void *data);
bool fdm_del(struct fdm *fdm, int fd);
bool fdm_del_no_close(struct fdm *fdm, int fd);

bool fdm_event_add(struct fdm *fdm, int fd, int events);
bool fdm_event_del(struct fdm *fdm, int fd, int events);

bool fdm_poll(struct fdm *fdm);
