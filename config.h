#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <tllist.h>

#include "terminal.h"
#include "user-notification.h"
#include "wayland.h"

#ifdef HAVE_TERMINFO
	#define DEFAULT_TERM "foot"
#else
	#define DEFAULT_TERM "xterm-256color"
#endif

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

struct config_binding_pipe {
    char *cmd;
    char **argv;
    bool master_copy;
};

struct config_key_binding {
    int action;  /* One of the varios bind_action_* enums from wayland.h */
    struct config_key_modifiers modifiers;
    xkb_keysym_t sym;
    struct config_binding_pipe pipe;
};
typedef tll(struct config_key_binding) config_key_binding_list_t;

struct config_mouse_binding {
    enum bind_action_normal action;
    struct config_key_modifiers modifiers;
    int button;
    int count;
    struct config_binding_pipe pipe;
};

struct config_spawn_template {
    char *raw_cmd;
    char **argv;
};

struct config {
    char *term;
    char *shell;
    char *title;
    char *app_id;
    wchar_t *word_delimiters;
    wchar_t *jump_label_letters;
    bool login_shell;
    bool no_wait;

    struct {
        enum conf_size_type type;
        unsigned width;
        unsigned height;
    } size;

    unsigned pad_x;
    unsigned pad_y;
    bool center;
    uint16_t resize_delay_ms;

    bool bold_in_bright;
    enum {
        BELL_ACTION_NONE,
        BELL_ACTION_URGENT,
        BELL_ACTION_NOTIFY,
    } bell_action;

    enum { STARTUP_WINDOWED, STARTUP_MAXIMIZED, STARTUP_FULLSCREEN } startup_mode;

    enum {DPI_AWARE_AUTO, DPI_AWARE_YES, DPI_AWARE_NO} dpi_aware;
    config_font_list_t fonts[4];

    /* Custom font metrics (-1 = use real font metrics) */
    struct pt_or_px line_height;
    struct pt_or_px letter_spacing;

    /* Adjusted letter x/y offsets */
    struct pt_or_px horizontal_letter_offset;
    struct pt_or_px vertical_letter_offset;

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
        uint32_t url;

        struct {
            uint32_t fg;
            uint32_t bg;
        } jump_label;

        struct {
            bool selection:1;
            bool jump_label:1;
            bool url:1;
        } use_custom;
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
        config_key_binding_list_t key;
        tll(struct config_mouse_binding) mouse;

        /*
         * Special modes
         */

        /* While searching (not - action to *start* a search is in the
         * 'key' bindings above */
        config_key_binding_list_t search;

        /* While showing URL jump labels */
        config_key_binding_list_t url;
    } bindings;

    struct {
        enum { CONF_CSD_PREFER_NONE, CONF_CSD_PREFER_SERVER, CONF_CSD_PREFER_CLIENT } preferred;

        int title_height;
        int border_width;
        int button_width;

        struct {
            bool title_set:1;
            bool minimize_set:1;
            bool maximize_set:1;
            bool close_set:1;
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

    struct config_spawn_template notify;
    struct config_spawn_template url_launch;

    enum {
        OSC8_UNDERLINE_URL_MODE,
        OSC8_UNDERLINE_ALWAYS,
    } osc8_underline;

    struct {
        enum fcft_scaling_filter fcft_filter;
        bool allow_overflowing_double_width_glyphs;
        bool render_timer_osd;
        bool render_timer_log;
        bool damage_whole_window;
        uint64_t delayed_render_lower_ns;
        uint64_t delayed_render_upper_ns;
        off_t max_shm_pool_size;
        float box_drawing_base_thickness;
    } tweak;

    user_notifications_t notifications;
};

bool config_load(
    struct config *conf, const char *path,
    user_notifications_t *initial_user_notifications, bool errors_are_fatal);
void config_free(struct config conf);

bool config_font_parse(const char *pattern, struct config_font *font);
void config_font_destroy(struct config_font *font);
