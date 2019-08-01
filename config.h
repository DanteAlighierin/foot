#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "terminal.h"
#include "tllist.h"

struct config {
    char *term;
    char *shell;
    tll(char *) fonts;

    int scrollback_lines;

    struct {
        uint32_t fg;
        uint32_t bg;
        uint32_t regular[8];
        uint32_t bright[8];
    } colors;

    struct {
        enum cursor_style style;
        struct {
            uint32_t text;
            uint32_t cursor;
        } color;
    } cursor;

    size_t render_worker_count;
};

bool config_load(struct config *conf);
void config_free(struct config conf);
