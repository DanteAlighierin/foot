#pragma once

#include <stdbool.h>
#include <sys/wait.h>

#include "fdm.h"

struct reaper;

struct reaper *reaper_init(struct fdm *fdm);
void reaper_destroy(struct reaper *reaper);

typedef void (*reaper_cb)(
    struct reaper *reaper, pid_t pid, int status, void *data);

void reaper_add(struct reaper *reaper, pid_t pid, reaper_cb cb, void *cb_data);
void reaper_del(struct reaper *reaper, pid_t pid);
