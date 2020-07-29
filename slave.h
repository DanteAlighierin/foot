#pragma once
#include <stdbool.h>

#include <sys/types.h>

#include "terminal.h"

pid_t slave_spawn(
    int ptmx, int argc, const char *cwd, char *const *argv, const char *term_env,
    const char *conf_shell, bool login_shell,
    const user_warning_list_t *warnings);
