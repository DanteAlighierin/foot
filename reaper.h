#pragma once

#include <stdbool.h>
#include <sys/wait.h>

#include "fdm.h"

struct reaper;

struct reaper *reaper_init(struct fdm *fdm);
void reaper_destroy(struct reaper *reaper);

typedef bool (*reaper_cb)(struct reaper *reaper, pid_t pid, void *data);

void reaper_add(struct reaper *reaper, pid_t pid, reaper_cb cb, void *cb_data);
