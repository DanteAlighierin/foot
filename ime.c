#include "ime.h"

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED

#include <string.h>

#include "text-input-unstable-v3.h"

#define LOG_MODULE "ime"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "render.h"
#include "search.h"
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

    /* The main grid is the *only* input-receiving surface we have */
    /* TODO: can we receive text_input::enter() _before_ keyboard_enter()? */
    struct terminal UNUSED *term = seat->kbd_focus;
    xassert(term != NULL);
    xassert(term_surface_kind(term, surface) == TERM_SURF_GRID);

    ime_enable(seat);
}

static void
leave(void *data, struct zwp_text_input_v3 *zwp_text_input_v3,
      struct wl_surface *surface)
{
    struct seat *seat = data;
    LOG_DBG("leave: seat=%s", seat->name);
    ime_disable(seat);
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

    xassert(seat->kbd_focus);
    struct terminal *term = seat->kbd_focus;

    /* 1. Delete existing pre-edit text */
    if (term->ime.preedit.cells != NULL) {
        term_ime_reset(term);
        if (term->is_searching)
            render_refresh_search(term);
        else
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
        const char *text = seat->ime.commit.pending.text;
        size_t len = strlen(text);

        if (term->is_searching) {
            search_add_chars(term, text, len);
            render_refresh_search(term);
        } else
            term_to_slave(term, text, len);
        ime_reset_commit(seat);
    }

    /* 4. Calculate surrounding text to send - not supported */

    /* 5. Insert new pre-edit text */
    size_t wchars = seat->ime.preedit.pending.text != NULL
        ? mbstowcs(NULL, seat->ime.preedit.pending.text, 0)
        : 0;

    if (wchars == 0 || wchars == (size_t)-1) {
        ime_reset_preedit(seat);
        return;
    }

    /* First, convert to unicode */
    term->ime.preedit.text = xmalloc((wchars + 1) * sizeof(wchar_t));
    mbstowcs(term->ime.preedit.text, seat->ime.preedit.pending.text, wchars);
    term->ime.preedit.text[wchars] = L'\0';

    /* Next, count number of cells needed */
    size_t cell_count = 0;
    size_t widths[wchars + 1];

    for (size_t i = 0; i < wchars; i++) {
        int width = max(wcwidth(term->ime.preedit.text[i]), 1);
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

        cell->wc = term->ime.preedit.text[i];
        cell->attrs = (struct attributes){.clean = 0};

        for (int j = 1; j < width; j++) {
            cell = &term->ime.preedit.cells[cell_idx + j];
            cell->wc = CELL_MULT_COL_SPACER;
            cell->attrs = (struct attributes){.clean = 1};
        }

        cell_idx += width;
    }

    const size_t byte_len = strlen(seat->ime.preedit.pending.text);

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

    else if (seat->ime.preedit.pending.cursor_begin == byte_len &&
             seat->ime.preedit.pending.cursor_end == byte_len)
    {
        /* Cursor is *after* the entire pre-edit string */
        term->ime.preedit.cursor.hidden = false;
        term->ime.preedit.cursor.start = cell_count;
        term->ime.preedit.cursor.end = cell_count;
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
         */

        int cell_begin = -1, cell_end = -1;
        for (size_t byte_idx = 0, wc_idx = 0, cell_idx = 0;
             byte_idx < byte_len &&
                 wc_idx < wchars &&
                 cell_idx < cell_count &&
                 (cell_begin < 0 || cell_end < 0);
             cell_idx += widths[wc_idx], wc_idx++)
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
        }

        if (seat->ime.preedit.pending.cursor_end >= byte_len)
            cell_end = cell_count;

        /* Bounded by number of screen columns */
        cell_begin = min(max(cell_begin, 0), cell_count - 1);
        cell_end = min(max(cell_end, 0), cell_count);

        if (cell_end < cell_begin)
            cell_end = cell_begin;

        /* Expand cursor end to end of glyph */
        while (cell_end > cell_begin && cell_end < cell_count &&
               term->ime.preedit.cells[cell_end].wc == CELL_MULT_COL_SPACER)
        {
            cell_end++;
        }

        LOG_DBG("pre-edit cursor: begin=%d, end=%d", cell_begin, cell_end);

        xassert(cell_begin >= 0);
        xassert(cell_begin < cell_count);
        xassert(cell_begin <= cell_end);
        xassert(cell_end >= 0);
        xassert(cell_end <= cell_count);

        term->ime.preedit.cursor.hidden = false;
        term->ime.preedit.cursor.start = cell_begin;
        term->ime.preedit.cursor.end = cell_end;
    }

    /* Underline pre-edit string that is *not* covered by the cursor */
    bool hidden = term->ime.preedit.cursor.hidden;
    int start = term->ime.preedit.cursor.start;
    int end = term->ime.preedit.cursor.end;

    for (size_t i = 0, cell_idx = 0; i < wchars; cell_idx += widths[i], i++) {
        if (hidden || start == end || cell_idx < start || cell_idx >= end) {
            struct cell *cell = &term->ime.preedit.cells[cell_idx];
            cell->attrs.underline = true;
        }
    }

    ime_reset_preedit(seat);

    if (term->is_searching)
        render_refresh_search(term);
    else
        render_refresh(term);
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

void
ime_send_cursor_rect(struct seat *seat, struct terminal *term)
{
    if (unlikely(seat->wayl->text_input_manager == NULL))
        return;

    if (!term->ime.enabled)
        return;

    if (seat->ime.cursor_rect.pending.x == seat->ime.cursor_rect.sent.x &&
        seat->ime.cursor_rect.pending.y == seat->ime.cursor_rect.sent.y &&
        seat->ime.cursor_rect.pending.width == seat->ime.cursor_rect.sent.width &&
        seat->ime.cursor_rect.pending.height == seat->ime.cursor_rect.sent.height)
    {
        return;
    }

    zwp_text_input_v3_set_cursor_rectangle(
        seat->wl_text_input,
        seat->ime.cursor_rect.pending.x / term->scale,
        seat->ime.cursor_rect.pending.y / term->scale,
        seat->ime.cursor_rect.pending.width / term->scale,
        seat->ime.cursor_rect.pending.height / term->scale);

    zwp_text_input_v3_commit(seat->wl_text_input);
    seat->ime.serial++;

    seat->ime.cursor_rect.sent = seat->ime.cursor_rect.pending;
}

void
ime_enable(struct seat *seat)
{
    if (unlikely(seat->wayl->text_input_manager == NULL))
        return;

    struct terminal *term = seat->kbd_focus;
    xassert(term != NULL);

    if (!term->ime.enabled)
        return;

    ime_reset(seat);

    zwp_text_input_v3_enable(seat->wl_text_input);
    zwp_text_input_v3_set_content_type(
        seat->wl_text_input,
        ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
        ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);

    zwp_text_input_v3_set_cursor_rectangle(
        seat->wl_text_input,
        seat->ime.cursor_rect.pending.x / term->scale,
        seat->ime.cursor_rect.pending.y / term->scale,
        seat->ime.cursor_rect.pending.width / term->scale,
        seat->ime.cursor_rect.pending.height / term->scale);

    seat->ime.cursor_rect.sent = seat->ime.cursor_rect.pending;

    zwp_text_input_v3_commit(seat->wl_text_input);
    seat->ime.serial++;
}

void
ime_disable(struct seat *seat)
{
    if (unlikely(seat->wayl->text_input_manager == NULL))
        return;

    ime_reset(seat);

    zwp_text_input_v3_disable(seat->wl_text_input);
    zwp_text_input_v3_commit(seat->wl_text_input);
    seat->ime.serial++;
}

void
ime_update_cursor_rect(struct seat *seat, struct terminal *term)
{
    /* Set in render_ime_preedit() */
    if (term->ime.preedit.cells != NULL)
        goto update;

    /* Set in render_search_box() */
    if (term->is_searching)
        goto update;

    int x, y, width, height;
    int col = term->grid->cursor.point.col;
    int row = term->grid->cursor.point.row;
    row += term->grid->offset;
    row -= term->grid->view;
    row &= term->grid->num_rows - 1;
    x = term->margins.left + col * term->cell_width;
    y = term->margins.top + row * term->cell_height;

    if (term->cursor_style == CURSOR_BAR)
        width = 1;
    else
        width = term->cell_width;

    height = term->cell_height;

    seat->ime.cursor_rect.pending.x = x;
    seat->ime.cursor_rect.pending.y = y;
    seat->ime.cursor_rect.pending.width = width;
    seat->ime.cursor_rect.pending.height = height;

update:
    ime_send_cursor_rect(seat, term);
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

void ime_enable(struct seat *seat) {}
void ime_disable(struct seat *seat) {}
void ime_update_cursor_rect(struct seat *seat, struct terminal *term) {}

void ime_reset_preedit(struct seat *seat) {}
void ime_reset_commit(struct seat *seat) {}
void ime_reset(struct seat *seat) {}
void ime_send_cursor_rect(struct seat *seat, struct terminal *term) {}

#endif
