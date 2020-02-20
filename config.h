#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <tllist.h>

#include "terminal.h"

struct config {
    char *term;
    char *shell;
    bool login_shell;
    unsigned width;
    unsigned height;
    unsigned pad_x;
    unsigned pad_y;

    tll(char *) fonts;

    int scrollback_lines;

    struct {
        uint32_t fg;
        uint32_t bg;
        uint32_t regular[8];
        uint32_t bright[8];
        uint16_t alpha;
    } colors;

    struct {
        enum cursor_style style;
        struct {
            uint32_t text;
            uint32_t cursor;
        } color;
    } cursor;

    size_t render_worker_count;
    char *server_socket_path;
    bool presentation_timings;
    bool hold_at_exit;
};

bool config_load(struct config *conf, const char *path);
void config_free(struct config conf);
