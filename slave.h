#pragma once
#include <stdbool.h>

void slave_exec(int ptmx, char *const argv[], int err_fd);
