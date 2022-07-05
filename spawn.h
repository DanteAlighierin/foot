#pragma once

#include <stdbool.h>
#include "config.h"
#include "reaper.h"

bool spawn(struct reaper *reaper, const char *cwd, char *const argv[],
           int stdin_fd, int stdout_fd, int stderr_fd,
           const char *xdg_activation_token);

bool spawn_expand_template(
    const struct config_spawn_template *template,
    size_t key_count, const char *key_names[static key_count],
    const char *key_values[static key_count], size_t *argc, char ***argv);
