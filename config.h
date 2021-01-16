#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <tllist.h>

#include "terminal.h"
#include "user-notification.h"
#include "wayland.h"

enum conf_size_type {CONF_SIZE_PX, CONF_SIZE_CELLS};

struct config_font {
    char *pattern;
    double pt_size;
    int px_size;
};
typedef tll(struct config_font) config_font_list_t;

struct config_key_modifiers {
    bool shift;
    bool alt;
    bool ctrl;
    bool meta;
};

struct config_key_binding_normal {
    enum bind_action_normal action;
    struct config_key_modifiers modifiers;
    xkb_keysym_t sym;
    struct {
        char *cmd;
        char **argv;
        bool master_copy;
    } pipe;
};

struct config_key_binding_search {
    enum bind_action_search action;
    struct config_key_modifiers modifiers;
    xkb_keysym_t sym;
};

struct config_mouse_binding {
    enum bind_action_normal action;
    struct config_key_modifiers modifiers;
    int button;
    int count;
    struct {
        char *cmd;
        char **argv;
        bool master_copy;
    } pipe;
};

struct config {
    char *term;
    char *shell;
    char *title;
    char *app_id;
    wchar_t *word_delimiters;
    bool login_shell;

    struct {
        enum conf_size_type type;
        unsigned width;
        unsigned height;
    } size;

    unsigned pad_x;
    unsigned pad_y;

    bool bold_in_bright;
    enum {
        BELL_ACTION_NONE,
        BELL_ACTION_URGENT,
        BELL_ACTION_NOTIFY,
    } bell_action;

    enum { STARTUP_WINDOWED, STARTUP_MAXIMIZED, STARTUP_FULLSCREEN } startup_mode;

    enum {DPI_AWARE_AUTO, DPI_AWARE_YES, DPI_AWARE_NO} dpi_aware;
    config_font_list_t fonts[4];

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
        bool alternate_scroll_mode;
    } mouse;

    struct {
        /* Bindings for "normal" mode */
        tll(struct config_key_binding_normal) key;
        tll(struct config_mouse_binding) mouse;

        /*
         * Special modes
         */

        /* While searching (not - action to *start* a search is in the
         * 'key' bindings above */
        tll(struct config_key_binding_search) search;
    } bindings;

    struct {
        enum { CONF_CSD_PREFER_NONE, CONF_CSD_PREFER_SERVER, CONF_CSD_PREFER_CLIENT } preferred;

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
    enum {
        SELECTION_TARGET_NONE,
        SELECTION_TARGET_PRIMARY,
        SELECTION_TARGET_CLIPBOARD,
        SELECTION_TARGET_BOTH
    } selection_target;

    struct {
        char *raw_cmd;
        char **argv;
    } notify;

    struct {
        enum fcft_scaling_filter fcft_filter;
        bool allow_overflowing_double_width_glyphs;
        bool render_timer_osd;
        bool render_timer_log;
        bool damage_whole_window;
        uint64_t delayed_render_lower_ns;
        uint64_t delayed_render_upper_ns;
        off_t max_shm_pool_size;
    } tweak;

    user_notifications_t notifications;
};

bool config_load(
    struct config *conf, const char *path,
    user_notifications_t *initial_user_notifications, bool errors_are_fatal);
void config_free(struct config conf);

bool config_font_parse(const char *pattern, struct config_font *font);
void config_font_destroy(struct config_font *font);
