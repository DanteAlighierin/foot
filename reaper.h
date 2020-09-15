#pragma once

#include <sys/wait.h>

#include "fdm.h"

struct reaper;

struct reaper *reaper_init(struct fdm *fdm);
void reaper_destroy(struct reaper *reaper);

void reaper_add(struct reaper *reaper, pid_t pid);
