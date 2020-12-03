#include "ime.h"

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED

#include <string.h>

#include "text-input-unstable-v3.h"

#define LOG_MODULE "ime"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "render.h"
#include "terminal.h"
#include "util.h"
#include "wayland.h"
#include "xmalloc.h"

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
    seat->ime.serial++;
}

static void
leave(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
      struct wl_surface *surface)
{
    struct seat *seat = data;
    LOG_DBG("leave: seat=%s", seat->name);
    zwp_text_input_v3_disable(seat->wl_text_input);
    zwp_text_input_v3_commit(seat->wl_text_input);
    seat->ime.serial++;

    ime_reset(seat);
}

static void
preedit_string(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
               const char *text, int32_t cursor_begin, int32_t cursor_end)
{
    LOG_DBG("preedit-string: text=%s, begin=%d, end=%d", text, cursor_begin, cursor_end);

    struct seat *seat = data;

    ime_reset_preedit(seat);

    if (text != NULL) {
        seat->ime.preedit.pending.text = xstrdup(text);
        seat->ime.preedit.pending.cursor_begin = cursor_begin;
        seat->ime.preedit.pending.cursor_end = cursor_end;
    }
}

static void
commit_string(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
              const char *text)
{
    LOG_DBG("commit: text=%s", text);

    struct seat *seat = data;

    ime_reset_commit(seat);

    if (text != NULL)
        seat->ime.commit.pending.text = xstrdup(text);
}

static void
delete_surrounding_text(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
                        uint32_t before_length, uint32_t after_length)
{
    LOG_DBG("delete-surrounding: before=%d, after=%d", before_length, after_length);

    struct seat *seat = data;
    seat->ime.surrounding.pending.before_length = before_length;
    seat->ime.surrounding.pending.after_length = after_length;
}

static void
done(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
     uint32_t serial)
{
    /*
     * From text-input-unstable-v3.h:
     *
     * The application must proceed by evaluating the changes in the
     * following order:
     *
     * 1. Replace existing preedit string with the cursor.
     * 2. Delete requested surrounding text.
     * 3. Insert commit string with the cursor at its end.
     * 4. Calculate surrounding text to send.
     * 5. Insert new preedit text in cursor position.
     * 6. Place cursor inside preedit text.
     */

    LOG_DBG("done: serial=%u", serial);
    struct seat *seat = data;

    if (seat->ime.serial != serial) {
        LOG_DBG("IME serial mismatch: expected=0x%08x, got 0x%08x",
                seat->ime.serial, serial);
        return;
    }

    assert(seat->kbd_focus);
    struct terminal *term = seat->kbd_focus;

    /* 1. Delete existing pre-edit text */
    if (term->ime.preedit.cells != NULL) {
        term_reset_ime(term);
        render_refresh(term);
    }

    /*
     * 2. Delete requested surroundin text
     *
     * We don't support deleting surrounding text. But, we also never
     * call set_surrounding_text() so hopefully we should never
     * receive any requests to delete surrounding text.
     */

    /* 3. Insert commit string */
    if (seat->ime.commit.pending.text != NULL) {
        term_to_slave(
            term,
            seat->ime.commit.pending.text,
            strlen(seat->ime.commit.pending.text));
        ime_reset_commit(seat);
    }

    /* 4. Calculate surrounding text to send - not supported */

    /* 5. Insert new pre-edit text */
    size_t wchars = seat->ime.preedit.pending.text != NULL
        ? mbstowcs(NULL, seat->ime.preedit.pending.text, 0)
        : 0;

    if (wchars > 0) {
        /* First, convert to unicode */
        wchar_t wcs[wchars + 1];
        mbstowcs(wcs, seat->ime.preedit.pending.text, wchars);

        /* Next, count number of cells needed */
        size_t cell_count = 0;
        size_t widths[wchars + 1];

        for (size_t i = 0; i < wchars; i++) {
            int width = max(wcwidth(wcs[i]), 1);
            widths[i] = width;
            cell_count += width;
        }

        /* Allocate cells */
        term->ime.preedit.cells = xmalloc(
            cell_count * sizeof(term->ime.preedit.cells[0]));
        term->ime.preedit.count = cell_count;

        /* Populate cells */
        for (size_t i = 0, cell_idx = 0; i < wchars; i++) {
            struct cell *cell = &term->ime.preedit.cells[cell_idx];

            int width = widths[i];

            cell->wc = wcs[i];
            cell->attrs = (struct attributes){.clean = 0, .underline = 1};

            for (int j = 1; j < width; j++) {
                cell = &term->ime.preedit.cells[cell_idx + j];
                cell->wc = CELL_MULT_COL_SPACER;
                cell->attrs = (struct attributes){.clean = 1};
            }

            cell_idx += width;
        }

        /* Pre-edit cursor - hidden */
        if (seat->ime.preedit.pending.cursor_begin == -1 ||
            seat->ime.preedit.pending.cursor_end == -1)
        {
            /* Note: docs says *both* begin and end should be -1,
             * but what else can we do if only one is -1? */
            LOG_DBG("pre-edit cursor is hidden");
            term->ime.preedit.cursor.hidden = true;
            term->ime.preedit.cursor.start = -1;
            term->ime.preedit.cursor.end = -1;
        }

        else {
            /*
             * Translate cursor position to cell indices
             *
             * The cursor_begin and cursor_end are counted in
             * *bytes*. We want to map them to *cell* indices.
             *
             * To do this, we use mblen() to step though the utf-8
             * pre-edit string, advancing a unicode character index as
             * we go, *and* advancing a *cell* index using wcwidth()
             * of the unicode character.
             *
             * When we find the matching *byte* index, we at the same
             * time know both the unicode *and* cell index.
             *
             * Note that this has only been tested with
             *
             *   cursor_begin == cursor_end == 0
             *
             * I haven't found an IME that requests anything else
             */
            
            const size_t byte_len = strlen(seat->ime.preedit.pending.text);

            int cell_begin = -1, cell_end = -1;
            for (size_t byte_idx = 0, wc_idx = 0, cell_idx = 0;
                 byte_idx < byte_len &&
                     wc_idx < wchars &&
                     cell_idx < cell_count &&
                     (cell_begin < 0 || cell_end < 0);
                 )
            {
                if (seat->ime.preedit.pending.cursor_begin == byte_idx)
                    cell_begin = cell_idx;
                if (seat->ime.preedit.pending.cursor_end == byte_idx)
                    cell_end = cell_idx;

                /* Number of bytes of *next* utf-8 character */
                size_t left = byte_len - byte_idx;
                int wc_bytes = mblen(&seat->ime.preedit.pending.text[byte_idx], left);

                if (wc_bytes <= 0)
                    break;

                byte_idx += wc_bytes;
                cell_idx += max(wcwidth(term->ime.preedit.cells[wc_idx].wc), 1);
                wc_idx++;
            }

            /* Bounded by number of screen columns */
            cell_begin = min(max(cell_begin, 0), cell_count - 1);

            /* Ensure end comes *after* begin, and is bounded by screen */
            if (cell_end <= cell_begin)
                cell_end = cell_begin + max(wcwidth(term->ime.preedit.cells[cell_begin].wc), 1);
            cell_end = min(max(cell_end, 0), cell_count);

            LOG_DBG("pre-edit cursor: begin=%d, end=%d", cell_begin, cell_end);

            assert(cell_begin >= 0);
            assert(cell_begin < cell_count);
            assert(cell_end >= 1);
            assert(cell_end <= cell_count);
            assert(cell_begin < cell_end);

            term->ime.preedit.cursor.hidden = false;
            term->ime.preedit.cursor.start = cell_begin;
            term->ime.preedit.cursor.end = cell_end;
        }

        render_refresh(term);
    }

    ime_reset_preedit(seat);
}

void
ime_reset_preedit(struct seat *seat)
{
    free(seat->ime.preedit.pending.text);
    seat->ime.preedit.pending.text = NULL;
}

void
ime_reset_commit(struct seat *seat)
{
    free(seat->ime.commit.pending.text);
    seat->ime.commit.pending.text = NULL;
}

void
ime_reset(struct seat *seat)
{
    ime_reset_preedit(seat);
    ime_reset_commit(seat);
}


const struct zwp_text_input_v3_listener text_input_listener = {
    .enter = &enter,
    .leave = &leave,
    .preedit_string = &preedit_string,
    .commit_string = &commit_string,
    .delete_surrounding_text = &delete_surrounding_text,
    .done = &done,
};

#else /* !FOOT_IME_ENABLED */

void ime_reset_preedit(struct seat *seat) {}
void ime_reset_commit(struct seat *seat) {}
void ime_reset(struct seat *seat) {}

#endif
