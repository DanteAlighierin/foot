#pragma once

#include <stdbool.h>
#include "reaper.h"

bool spawn(struct reaper *reaper, const char *cwd, char *const argv[]);
