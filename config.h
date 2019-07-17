#pragma once

#include <stdbool.h>

struct config {
    char *shell;
    char *font;
};

bool config_load(struct config *conf);
void config_free(struct config conf);
