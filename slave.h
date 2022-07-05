#pragma once
#include <stdbool.h>

#include <sys/types.h>

#include "config.h"
#include "user-notification.h"

pid_t slave_spawn(
    int ptmx, int argc, const char *cwd, char *const *argv, char *const *envp,
    const env_var_list_t *extra_env_vars, const char *term_env,
    const char *conf_shell, bool login_shell,
    const user_notifications_t *notifications);
