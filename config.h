#pragma once

#include <stdint.h>
#include <stdbool.h>

struct config {
    char *term;
    char *shell;
    char *font;

    struct {
        uint32_t fg;
        uint32_t bg;
        uint32_t regular[8];
        uint32_t bright[8];
    } colors;
};

bool config_load(struct config *conf);
void config_free(struct config conf);
