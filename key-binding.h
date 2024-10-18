#pragma once

#include <stdint.h>

#include <xkbcommon/xkbcommon.h>
#include <tllist.h>

#include "config.h"

enum bind_action_normal {
    BIND_ACTION_NONE,
    BIND_ACTION_NOOP,
    BIND_ACTION_SCROLLBACK_UP_PAGE,
    BIND_ACTION_SCROLLBACK_UP_HALF_PAGE,
    BIND_ACTION_SCROLLBACK_UP_LINE,
    BIND_ACTION_SCROLLBACK_DOWN_PAGE,
    BIND_ACTION_SCROLLBACK_DOWN_HALF_PAGE,
    BIND_ACTION_SCROLLBACK_DOWN_LINE,
    BIND_ACTION_SCROLLBACK_HOME,
    BIND_ACTION_SCROLLBACK_END,
    BIND_ACTION_CLIPBOARD_COPY,
    BIND_ACTION_CLIPBOARD_PASTE,
    BIND_ACTION_PRIMARY_PASTE,
    BIND_ACTION_SEARCH_START,
    BIND_ACTION_FONT_SIZE_UP,
    BIND_ACTION_FONT_SIZE_DOWN,
    BIND_ACTION_FONT_SIZE_RESET,
    BIND_ACTION_SPAWN_TERMINAL,
    BIND_ACTION_MINIMIZE,
    BIND_ACTION_MAXIMIZE,
    BIND_ACTION_FULLSCREEN,
    BIND_ACTION_PIPE_SCROLLBACK,
    BIND_ACTION_PIPE_VIEW,
    BIND_ACTION_PIPE_SELECTED,
    BIND_ACTION_PIPE_COMMAND_OUTPUT,
    BIND_ACTION_SHOW_URLS_COPY,
    BIND_ACTION_SHOW_URLS_LAUNCH,
    BIND_ACTION_SHOW_URLS_PERSISTENT,
    BIND_ACTION_TEXT_BINDING,
    BIND_ACTION_PROMPT_PREV,
    BIND_ACTION_PROMPT_NEXT,
    BIND_ACTION_UNICODE_INPUT,
    BIND_ACTION_QUIT,

    /* Mouse specific actions - i.e. they require a mouse coordinate */
    BIND_ACTION_SCROLLBACK_UP_MOUSE,
    BIND_ACTION_SCROLLBACK_DOWN_MOUSE,
    BIND_ACTION_SELECT_BEGIN,
    BIND_ACTION_SELECT_BEGIN_BLOCK,
    BIND_ACTION_SELECT_EXTEND,
    BIND_ACTION_SELECT_EXTEND_CHAR_WISE,
    BIND_ACTION_SELECT_WORD,
    BIND_ACTION_SELECT_WORD_WS,
    BIND_ACTION_SELECT_QUOTE,
    BIND_ACTION_SELECT_ROW,

    BIND_ACTION_KEY_COUNT = BIND_ACTION_QUIT + 1,
    BIND_ACTION_COUNT = BIND_ACTION_SELECT_ROW + 1,
};

enum bind_action_search {
    BIND_ACTION_SEARCH_NONE,
    BIND_ACTION_SEARCH_SCROLLBACK_UP_PAGE,
    BIND_ACTION_SEARCH_SCROLLBACK_UP_HALF_PAGE,
    BIND_ACTION_SEARCH_SCROLLBACK_UP_LINE,
    BIND_ACTION_SEARCH_SCROLLBACK_DOWN_PAGE,
    BIND_ACTION_SEARCH_SCROLLBACK_DOWN_HALF_PAGE,
    BIND_ACTION_SEARCH_SCROLLBACK_DOWN_LINE,
    BIND_ACTION_SEARCH_SCROLLBACK_HOME,
    BIND_ACTION_SEARCH_SCROLLBACK_END,
    BIND_ACTION_SEARCH_CANCEL,
    BIND_ACTION_SEARCH_COMMIT,
    BIND_ACTION_SEARCH_FIND_PREV,
    BIND_ACTION_SEARCH_FIND_NEXT,
    BIND_ACTION_SEARCH_EDIT_LEFT,
    BIND_ACTION_SEARCH_EDIT_LEFT_WORD,
    BIND_ACTION_SEARCH_EDIT_RIGHT,
    BIND_ACTION_SEARCH_EDIT_RIGHT_WORD,
    BIND_ACTION_SEARCH_EDIT_HOME,
    BIND_ACTION_SEARCH_EDIT_END,
    BIND_ACTION_SEARCH_DELETE_PREV,
    BIND_ACTION_SEARCH_DELETE_PREV_WORD,
    BIND_ACTION_SEARCH_DELETE_NEXT,
    BIND_ACTION_SEARCH_DELETE_NEXT_WORD,
    BIND_ACTION_SEARCH_EXTEND_CHAR,
    BIND_ACTION_SEARCH_EXTEND_WORD,
    BIND_ACTION_SEARCH_EXTEND_WORD_WS,
    BIND_ACTION_SEARCH_EXTEND_LINE_DOWN,
    BIND_ACTION_SEARCH_EXTEND_BACKWARD_CHAR,
    BIND_ACTION_SEARCH_EXTEND_BACKWARD_WORD,
    BIND_ACTION_SEARCH_EXTEND_BACKWARD_WORD_WS,
    BIND_ACTION_SEARCH_EXTEND_LINE_UP,
    BIND_ACTION_SEARCH_CLIPBOARD_PASTE,
    BIND_ACTION_SEARCH_PRIMARY_PASTE,
    BIND_ACTION_SEARCH_UNICODE_INPUT,
    BIND_ACTION_SEARCH_COUNT,
};

enum bind_action_url {
    BIND_ACTION_URL_NONE,
    BIND_ACTION_URL_CANCEL,
    BIND_ACTION_URL_TOGGLE_URL_ON_JUMP_LABEL,
    BIND_ACTION_URL_COUNT,
};

typedef tll(xkb_keycode_t) xkb_keycode_list_t;

struct key_binding {
    enum key_binding_type type;

    int action; /* enum bind_action_* */
    xkb_mod_mask_t mods;

    union {
        struct {
            xkb_keysym_t sym;
            xkb_keycode_list_t key_codes;
        } k;

        struct {
            uint32_t button;
            int count;
        } m;
    };

    const struct binding_aux *aux;
};
typedef tll(struct key_binding) key_binding_list_t;

struct terminal;
struct seat;
struct wayland;

struct key_binding_set {
    key_binding_list_t key;
    key_binding_list_t search;
    key_binding_list_t url;
    key_binding_list_t mouse;
    xkb_mod_mask_t selection_overrides;
};

struct key_binding_manager;

struct key_binding_manager *key_binding_manager_new(void);
void key_binding_manager_destroy(struct key_binding_manager *mgr);

void key_binding_new_for_seat(
    struct key_binding_manager *mgr, const struct seat *seat);

void key_binding_new_for_conf(
    struct key_binding_manager *mgr, const struct wayland *wayl,
    const struct config *conf);

/* Returns the set of key bindings associated with this seat/conf pair */
struct key_binding_set *key_binding_for(
    struct key_binding_manager *mgr, const struct config *conf,
    const struct seat *seat);

/* Remove all key bindings tied to the specified seat */
void key_binding_remove_seat(
    struct key_binding_manager *mgr, const struct seat *seat);

void key_binding_unref(
    struct key_binding_manager *mgr, const struct config *conf);

void key_binding_load_keymap(
    struct key_binding_manager *mgr, const struct seat *seat);
void key_binding_unload_keymap(
    struct key_binding_manager *mgr, const struct seat *seat);
