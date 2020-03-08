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

    struct {
        /* Bindings for "normal" mode */
        char *key[BIND_ACTION_COUNT];
        char *mouse[BIND_ACTION_COUNT];

        /*
         * Special modes
         */

        /* While searching (not - action to *start* a search is in the
         * 'key' bindings above */
        char *search[BIND_ACTION_COUNT];
    } bindings;

    struct {
        enum { CONF_CSD_PREFER_SERVER, CONF_CSD_PREFER_CLIENT } preferred;

        int title_height;
        int border_width;
        int button_width;

        struct {
            bool title_set;
            bool minimize_set;
            bool maximize_set;
            bool close_set;
            uint32_t title;
            uint32_t minimize;
            uint32_t maximize;
            uint32_t close;
        } color;
    } csd;

    size_t render_worker_count;
    char *server_socket_path;
    bool presentation_timings;
    bool hold_at_exit;
};

bool config_load(struct config *conf, const char *path);
void config_free(struct config conf);
