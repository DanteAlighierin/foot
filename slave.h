#pragma once
#include <stdbool.h>

#include <sys/types.h>

pid_t slave_spawn(
    int ptmx, int argc, const char *cwd, char *const *argv, const char *term_env,
    const char *conf_shell, bool login_shell);
