#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <uchar.h>

#include <xkbcommon/xkbcommon.h>
#include <tllist.h>
#include <fcft/fcft.h>

#include "user-notification.h"

#define DEFINE_LIST(type) \
    type##_list {         \
        size_t count;     \
        type *arr;        \
    }

/* If px != 0 then px is valid, otherwise pt is valid */
struct pt_or_px {
    int16_t px;
    float pt;
};

struct font_size_adjustment {
    struct pt_or_px pt_or_px;
    float percent;
};

enum cursor_style { CURSOR_BLOCK, CURSOR_UNDERLINE, CURSOR_BEAM };
enum cursor_unfocused_style {
    CURSOR_UNFOCUSED_UNCHANGED,
    CURSOR_UNFOCUSED_HOLLOW,
    CURSOR_UNFOCUSED_NONE
};

enum conf_size_type {CONF_SIZE_PX, CONF_SIZE_CELLS};

struct config_font {
    char *pattern;
    float pt_size;
    int px_size;
};
DEFINE_LIST(struct config_font);

#if 0
struct config_key_modifiers {
    bool shift;
    bool alt;
    bool ctrl;
    bool super;
};
#endif

struct argv {
    char **args;
};

enum binding_aux_type {
    BINDING_AUX_NONE,
    BINDING_AUX_PIPE,
    BINDING_AUX_TEXT,
};

struct binding_aux {
    enum binding_aux_type type;
    bool master_copy;

    union {
        struct argv pipe;

        struct {
            uint8_t *data;
            size_t len;
        } text;
    };
};

enum key_binding_type {
    KEY_BINDING,
    MOUSE_BINDING,
};

typedef tll(char *) config_modifier_list_t;

struct config_key_binding {
    int action;  /* One of the various bind_action_* enums from wayland.h */
    //struct config_key_modifiers modifiers;
    config_modifier_list_t modifiers;
    union {
        /* Key bindings */
        struct {
            xkb_keysym_t sym;
        } k;

        /* Mouse bindings */
        struct {
            int button;
            int count;
        } m;
    };

    struct binding_aux aux;

    /* For error messages in collision handling */
    const char *path;
    int lineno;
};
DEFINE_LIST(struct config_key_binding);

typedef tll(char *) config_override_t;

struct config_spawn_template {
    struct argv argv;
};

struct env_var {
    char *name;
    char *value;
};
typedef tll(struct env_var) env_var_list_t;

struct config {
    char *term;
    char *shell;
    char *title;
    char *app_id;
    char32_t *word_delimiters;
    bool login_shell;
    bool locked_title;

    struct {
        enum conf_size_type type;
        uint32_t width;
        uint32_t height;
    } size;

    unsigned pad_x;
    unsigned pad_y;
    bool center;

    bool resize_by_cells;
    bool resize_keep_grid;

    uint16_t resize_delay_ms;

    struct {
        bool enabled;
        bool palette_based;
        float amount;
    } bold_in_bright;

    enum { STARTUP_WINDOWED, STARTUP_MAXIMIZED, STARTUP_FULLSCREEN } startup_mode;

    bool dpi_aware;
    struct config_font_list fonts[4];
    struct font_size_adjustment font_size_adjustment;

    /* Custom font metrics (-1 = use real font metrics) */
    struct pt_or_px line_height;
    struct pt_or_px letter_spacing;

    /* Adjusted letter x/y offsets */
    struct pt_or_px horizontal_letter_offset;
    struct pt_or_px vertical_letter_offset;

    bool use_custom_underline_offset;
    struct pt_or_px underline_offset;
    struct pt_or_px underline_thickness;

    struct pt_or_px strikeout_thickness;

    bool box_drawings_uses_font_glyphs;
    bool can_shape_grapheme;

    struct {
        bool urgent;
        bool notify;
        bool flash;
        struct config_spawn_template command;
        bool command_focused;
    } bell;

    struct {
        uint32_t lines;

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

            char32_t *text;
        } indicator;
        float multiplier;
    } scrollback;

    struct {
        char32_t *label_letters;
        struct config_spawn_template launch;
        enum {
            OSC8_UNDERLINE_URL_MODE,
            OSC8_UNDERLINE_ALWAYS,
        } osc8_underline;

        char32_t **protocols;
        char32_t *uri_characters;
        size_t prot_count;
        size_t max_prot_len;
    } url;

    struct {
        uint32_t fg;
        uint32_t bg;
        uint32_t flash;
        uint32_t flash_alpha;
        uint32_t table[256];
        uint16_t alpha;
        uint32_t selection_fg;
        uint32_t selection_bg;
        uint32_t url;

        uint32_t dim[8];

        struct {
            uint32_t fg;
            uint32_t bg;
        } jump_label;

        struct {
            uint32_t fg;
            uint32_t bg;
        } scrollback_indicator;

        struct {
            struct {
                uint32_t fg;
                uint32_t bg;
            } no_match;

            struct {
                uint32_t fg;
                uint32_t bg;
            } match;
        } search_box;

        struct {
            bool selection:1;
            bool jump_label:1;
            bool scrollback_indicator:1;
            bool url:1;
            bool search_box_no_match:1;
            bool search_box_match:1;
            uint8_t dim;
        } use_custom;
    } colors;

    struct {
        enum cursor_style style;
        enum cursor_unfocused_style unfocused_style;
        struct {
            bool enabled;
            uint32_t rate_ms;
        } blink;
        struct {
            uint32_t text;
            uint32_t cursor;
        } color;
        struct pt_or_px beam_thickness;
        struct pt_or_px underline_thickness;
    } cursor;

    struct {
        bool hide_when_typing;
        bool alternate_scroll_mode;
        //struct config_key_modifiers selection_override_modifiers;
        config_modifier_list_t selection_override_modifiers;
    } mouse;

    struct {
        /* Bindings for "normal" mode */
        struct config_key_binding_list key;
        struct config_key_binding_list mouse;

        /*
         * Special modes
         */

        /* While searching (not - action to *start* a search is in the
         * 'key' bindings above */
        struct config_key_binding_list search;

        /* While showing URL jump labels */
        struct config_key_binding_list url;
    } bindings;

    struct {
        enum { CONF_CSD_PREFER_NONE, CONF_CSD_PREFER_SERVER, CONF_CSD_PREFER_CLIENT } preferred;

        uint16_t title_height;
        uint16_t border_width;
        uint16_t border_width_visible;
        uint16_t button_width;

        bool hide_when_maximized;
        bool double_click_to_maximize;

        struct {
            bool title_set:1;
            bool buttons_set:1;
            bool minimize_set:1;
            bool maximize_set:1;
            bool close_set:1;
            bool border_set:1;
            uint32_t title;
            uint32_t buttons;
            uint32_t minimize;
            uint32_t maximize;
            uint32_t quit;  /* 'close' collides with #define in epoll-shim */
            uint32_t border;
        } color;

        struct config_font_list font;
    } csd;

    uint16_t render_worker_count;
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
        struct config_spawn_template command;
        struct config_spawn_template command_action_arg;
        struct config_spawn_template close;
        bool inhibit_when_focused;
    } desktop_notifications;

    env_var_list_t env_vars;

    char *utmp_helper_path;

    struct {
        enum fcft_scaling_filter fcft_filter;
        bool overflowing_glyphs;
        bool grapheme_shaping;
        enum {
            GRAPHEME_WIDTH_WCSWIDTH,
            GRAPHEME_WIDTH_DOUBLE,
            GRAPHEME_WIDTH_MAX,
        } grapheme_width_method;
        enum {
            RENDER_TIMER_NONE,
            RENDER_TIMER_OSD,
            RENDER_TIMER_LOG,
            RENDER_TIMER_BOTH
        } render_timer;
        bool damage_whole_window;
        uint32_t delayed_render_lower_ns;
        uint32_t delayed_render_upper_ns;
        off_t max_shm_pool_size;
        float box_drawing_base_thickness;
        bool box_drawing_solid_shades;
        bool font_monospace_warn;
        bool sixel;
    } tweak;

    struct {
        uint32_t long_press_delay;
    } touch;

    user_notifications_t notifications;
};

bool config_override_apply(struct config *conf, config_override_t *overrides,
    bool errors_are_fatal);
bool config_load(
    struct config *conf, const char *path,
    user_notifications_t *initial_user_notifications,
    config_override_t *overrides, bool errors_are_fatal,
    bool as_server);
void config_free(struct config *conf);
struct config *config_clone(const struct config *old);

bool config_font_parse(const char *pattern, struct config_font *font);
void config_font_list_destroy(struct config_font_list *font_list);

#if 0
struct seat;
xkb_mod_mask_t
conf_modifiers_to_mask(
    const struct seat *seat, const struct config_key_modifiers *modifiers);
#endif
bool check_if_font_is_monospaced(
    const char *pattern, user_notifications_t *notifications);
