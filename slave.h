#pragma once
#include <stdbool.h>

void slave_spawn(int ptmx, char *const argv[], int err_fd);
