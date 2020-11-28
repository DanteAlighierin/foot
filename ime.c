#include "ime.h"

#include <string.h>

#include "text-input-unstable-v3.h"

#define LOG_MODULE "ime"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "terminal.h"
#include "wayland.h"

static void
enter(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
      struct wl_surface *surface)
{
    struct seat *seat = data;
    LOG_DBG("enter: seat=%s", seat->name);

    assert(seat->kbd_focus != NULL);

    switch (term_surface_kind(seat->kbd_focus, surface)) {
    case TERM_SURF_GRID:
        zwp_text_input_v3_enable(seat->wl_text_input);
        zwp_text_input_v3_set_content_type(
            seat->wl_text_input,
            ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
            ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);

        /* TODO: set cursor rectangle */
        zwp_text_input_v3_set_cursor_rectangle(seat->wl_text_input, 0, 0, 15, 15);
        break;

    case TERM_SURF_SEARCH:
        /* TODO */
        /* FALLTHROUGH */

    case TERM_SURF_NONE:
    case TERM_SURF_SCROLLBACK_INDICATOR:
    case TERM_SURF_RENDER_TIMER:
    case TERM_SURF_TITLE:
    case TERM_SURF_BORDER_LEFT:
    case TERM_SURF_BORDER_RIGHT:
    case TERM_SURF_BORDER_TOP:
    case TERM_SURF_BORDER_BOTTOM:
    case TERM_SURF_BUTTON_MINIMIZE:
    case TERM_SURF_BUTTON_MAXIMIZE:
    case TERM_SURF_BUTTON_CLOSE:
        zwp_text_input_v3_disable(seat->wl_text_input);
        break;
    }

    zwp_text_input_v3_commit(seat->wl_text_input);
}

static void
leave(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
      struct wl_surface *surface)
{
    struct seat *seat = data;
    LOG_DBG("leave: seat=%s", seat->name);
    zwp_text_input_v3_disable(seat->wl_text_input);
    zwp_text_input_v3_commit(seat->wl_text_input);
}

static void
preedit_string(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
               const char *text, int32_t cursor_begin, int32_t cursor_end)
{
    LOG_DBG("preedit-string: text=%s, begin=%d, end=%d", text, cursor_begin, cursor_end);
}

static void
commit_string(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
              const char *text)
{
    struct seat *seat = data;
    LOG_DBG("commit: text=%s", text);
    term_to_slave(seat->kbd_focus, text, strlen(text));
}

static void
delete_surrounding_text(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
                        uint32_t before_length, uint32_t after_length)
{
    LOG_DBG("delete-surrounding: before=%d, after=%d", before_length, after_length);
}

static void
done(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
     uint32_t serial)
{
    LOG_DBG("done: serial=%u", serial);
}

const struct zwp_text_input_v3_listener text_input_listener = {
    .enter = &enter,
    .leave = &leave,
    .preedit_string = &preedit_string,
    .commit_string = &commit_string,
    .delete_surrounding_text = &delete_surrounding_text,
    .done = &done,
};
