#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <tllist.h>

#include "terminal.h"
#include "user-notification.h"
#include "wayland.h"

struct config_font {
    char *pattern;
    double pt_size;
    int px_size;
};

struct config_key_binding_normal {
    enum bind_action_normal action;
    char *key;
    struct {
        char *cmd;
        char **argv;
    } pipe;
};

struct config_key_binding_search {
    enum bind_action_search action;
    char *key;
};

struct config {
    char *term;
    char *shell;
    char *title;
    char *app_id;
    bool login_shell;
    unsigned width;
    unsigned height;
    unsigned pad_x;
    unsigned pad_y;
    enum { STARTUP_WINDOWED, STARTUP_MAXIMIZED, STARTUP_FULLSCREEN } startup_mode;

    tll(struct config_font) fonts;

    struct {
        int lines;

        struct {
            enum {
                SCROLLBACK_INDICATOR_POSITION_NONE,
                SCROLLBACK_INDICATOR_POSITION_FIXED,
                SCROLLBACK_INDICATOR_POSITION_RELATIVE
            } position;

            enum {
                SCROLLBACK_INDICATOR_FORMAT_PERCENTAGE,
                SCROLLBACK_INDICATOR_FORMAT_LINENO,
                SCROLLBACK_INDICATOR_FORMAT_TEXT,
            } format;

            wchar_t *text;
        } indicator;
        double multiplier;
    } scrollback;

    struct {
        uint32_t fg;
        uint32_t bg;
        uint32_t regular[8];
        uint32_t bright[8];
        uint16_t alpha;
        uint32_t selection_fg;
        uint32_t selection_bg;
        bool selection_uses_custom_colors;
    } colors;

    struct {
        enum cursor_style style;
        bool blink;
        struct {
            uint32_t text;
            uint32_t cursor;
        } color;
    } cursor;

    struct {
        bool hide_when_typing;
    } mouse;

    struct {
        /* Bindings for "normal" mode */
        tll(struct config_key_binding_normal) key;
        tll(struct mouse_binding) mouse;

        /*
         * Special modes
         */

        /* While searching (not - action to *start* a search is in the
         * 'key' bindings above */
        tll(struct config_key_binding_search) search;
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

    struct {
        bool render_timer_osd;
        bool render_timer_log;
        uint64_t delayed_render_lower_ns;
        uint64_t delayed_render_upper_ns;
        off_t max_shm_pool_size;
    } tweak;

    user_notifications_t notifications;
};

bool config_load(struct config *conf, const char *path, bool errors_are_fatal);
void config_free(struct config conf);

struct config_font config_font_parse(const char *pattern);
void config_font_destroy(struct config_font *font);
