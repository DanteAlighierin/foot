#include "selection.h"

#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define LOG_MODULE "selection"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "render.h"
#include "grid.h"
#include "vt.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

static bool
selection_enabled(const struct terminal *term)
{
    return term->mouse_tracking == MOUSE_NONE;
}

static char *
extract_selection(const struct terminal *term)
{
    const struct coord *start = &term->selection.start;
    const struct coord *end = &term->selection.end;

    if (start->row > end->row || (start->row == end->row && start->col > end->col)) {
        const struct coord *tmp = start;
        start = end;
        end = tmp;
    }

    assert(start->row <= end->row);

    size_t max_cells = 0;
    if (start->row == end->row) {
        assert(start->col <= end->col);
        max_cells = end->col - start->col + 1;
    } else {
        max_cells = term->cols - start->col;
        max_cells += term->cols * (end->row - start->row - 1);
        max_cells += end->col + 1;
    }

    char *buf = malloc(max_cells * 4 + 1);
    int idx = 0;

    int start_col = start->col;
    for (int r = start->row; r < end->row; r++) {
        const struct row *row = grid_row_in_view(term->grid, r - term->grid->view);
        if (row == NULL)
            continue;

        /* TODO: replace '\0' with spaces, then trim lines */
        for (int col = start_col; col < term->cols; col++) {
            const struct cell *cell = &row->cells[col];
            if (cell->c[0] == '\0')
                continue;

            size_t len = strnlen(cell->c, 4);
            memcpy(&buf[idx], cell->c, len);
            idx += len;
        }

        if (r != end->row - 1)
            buf[idx++] = '\n';

        start_col = 0;
    }

    {
        const struct row *row = grid_row_in_view(term->grid, end->row - term->grid->view);
        for (int col = start_col; row != NULL && col <= end->col; col++) {
            const struct cell *cell = &row->cells[col];
            if (cell->c[0] == '\0')
                continue;

            size_t len = strnlen(cell->c, 4);
            memcpy(&buf[idx], cell->c, len);
            idx += len;
        }
    }

    buf[idx] = '\0';
    return buf;
}

void
selection_start(struct terminal *term, int col, int row)
{
    if (!selection_enabled(term))
        return;

    selection_cancel(term);

    LOG_DBG("selection started at %d,%d", row, col);
    term->selection.start = (struct coord){col, term->grid->view + row};
    term->selection.end = (struct coord){-1, -1};
}

void
selection_update(struct terminal *term, int col, int row)
{
    if (!selection_enabled(term))
        return;

    LOG_DBG("selection updated: start = %d,%d, end = %d,%d -> %d, %d",
            term->selection.start.row, term->selection.start.col,
            term->selection.end.row, term->selection.end.col,
            row, col);

    int start_row = term->selection.start.row;
    int old_end_row = term->selection.end.row;
    int new_end_row = term->grid->view + row;

    assert(start_row != -1);
    assert(new_end_row != -1);

    if (old_end_row == -1)
        old_end_row = new_end_row;

    int from = min(start_row, min(old_end_row, new_end_row));
    int to = max(start_row, max(old_end_row, new_end_row));

    term->selection.end = (struct coord){col, term->grid->view + row};

    assert(term->selection.start.row != -1 && term->selection.end.row != -1);
    term_damage_rows_in_view(term, from - term->grid->view, to - term->grid->view);

    if (term->frame_callback == NULL)
        grid_render(term);
}

static const struct zwp_primary_selection_source_v1_listener primary_selection_source_listener;

void
selection_finalize(struct terminal *term, uint32_t serial)
{
    if (!selection_enabled(term))
        return;

    if (term->selection.start.row == -1 || term->selection.end.row == -1)
        return;

    assert(term->selection.start.row != -1);
    assert(term->selection.end.row != -1);

    /* TODO: somehow share code with the clipboard equivalent */
    if (term->selection.primary.data_source != NULL) {
        /* Kill previous data source */
        struct primary *primary = &term->selection.primary;

        assert(primary->serial != 0);
        zwp_primary_selection_device_v1_set_selection(
            term->wl.primary_selection_device, NULL, primary->serial);
        zwp_primary_selection_source_v1_destroy(primary->data_source);
        free(primary->text);

        primary->data_source = NULL;
        primary->serial = 0;
    }

    struct primary *primary = &term->selection.primary;

    primary->data_source
        = zwp_primary_selection_device_manager_v1_create_source(
            term->wl.primary_selection_device_manager);

    if (primary->data_source == NULL) {
        LOG_ERR("failed to create clipboard data source");
        return;
    }

    /* Get selection as a string */
    primary->text = extract_selection(term);

    /* Configure source */
    zwp_primary_selection_source_v1_offer(primary->data_source, "text/plain;charset=utf-8");
    zwp_primary_selection_source_v1_add_listener(primary->data_source, &primary_selection_source_listener, term);
    zwp_primary_selection_device_v1_set_selection(term->wl.primary_selection_device, primary->data_source, serial);
    zwp_primary_selection_source_v1_set_user_data(primary->data_source, primary);

    /* Needed when sending the selection to other client */
    primary->serial = serial;
}

void
selection_cancel(struct terminal *term)
{
    if (!selection_enabled(term))
        return;

    LOG_DBG("selection cancelled: start = %d,%d end = %d,%d",
            term->selection.start.row, term->selection.start.col,
            term->selection.end.row, term->selection.end.col);

    int start_row = term->selection.start.row;
    int end_row = term->selection.end.row;

    term->selection.start = (struct coord){-1, -1};
    term->selection.end = (struct coord){-1, -1};

    if (start_row != -1 && end_row != -1) {
        term_damage_rows_in_view(
            term,
            min(start_row, end_row) - term->grid->view,
            max(start_row, end_row) - term->grid->view);

        if (term->frame_callback == NULL)
            grid_render(term);
    }
}

void
selection_mark_word(struct terminal *term, int col, int row, uint32_t serial)
{
    if (!selection_enabled(term))
        return;

    selection_cancel(term);

    struct coord start = {col, row};
    struct coord end = {col, row};

    const struct row *r = grid_row_in_view(term->grid, start.row);
    unsigned char c = r->cells[start.col].c[0];

    if (!(c == '\0' || isspace(c))) {
        while (true) {
            int next_col = start.col - 1;
            int next_row = start.row;

            /* Linewrap */
            if (next_col < 0) {
                next_col = term->cols - 1;
                if (--next_row < 0)
                    break;
            }

            const struct row *row = grid_row_in_view(term->grid, next_row);

            unsigned char c = row->cells[next_col].c[0];
            if (c == '\0' || isspace(c))
                break;

            start.col = next_col;
            start.row = next_row;
        }
    }

    r = grid_row_in_view(term->grid, end.row);
    c = r->cells[end.col].c[0];

    if (!(c == '\0' || isspace(c))) {
        while (true) {
            int next_col = end.col + 1;
            int next_row = end.row;

            /* Linewrap */
            if (next_col >= term->cols) {
                next_col = 0;
                if (++next_row >= term->rows)
                    break;
            }

            const struct row *row = grid_row_in_view(term->grid, next_row);

            unsigned char c = row->cells[next_col].c[0];
            if (c == '\0' || isspace(c))
                break;

            end.col = next_col;
            end.row = next_row;
        }
    }

    selection_start(term, start.col, start.row);
    selection_update(term, end.col, end.row);
    selection_finalize(term, serial);
}

static void
target(void *data, struct wl_data_source *wl_data_source, const char *mime_type)
{
    LOG_WARN("TARGET: mime-type=%s", mime_type);
}

static void
send(void *data, struct wl_data_source *wl_data_source, const char *mime_type,
     int32_t fd)
{
    const struct clipboard *clipboard
        = wl_data_source_get_user_data(wl_data_source);

    assert(clipboard != NULL);
    assert(clipboard->text != NULL);

    size_t left = strlen(clipboard->text);
    size_t idx = 0;

    while (left > 0) {
        ssize_t ret = write(fd, &clipboard->text[idx], left);

        if (ret == -1 && errno != EINTR) {
            LOG_ERRNO("failed to write to clipboard");
            break;
        }

        left -= ret;
        idx += ret;
    }

    close(fd);
}

static void
cancelled(void *data, struct wl_data_source *wl_data_source)
{
    struct clipboard *clipboard = wl_data_source_get_user_data(wl_data_source);
    assert(clipboard->data_source == wl_data_source);

    wl_data_source_destroy(clipboard->data_source);
    clipboard->data_source = NULL;
    clipboard->serial = 0;

    free(clipboard->text);
    clipboard->text = NULL;
}

static void
dnd_drop_performed(void *data, struct wl_data_source *wl_data_source)
{
}

static void
dnd_finished(void *data, struct wl_data_source *wl_data_source)
{
}

static void
action(void *data, struct wl_data_source *wl_data_source, uint32_t dnd_action)
{
}

static const struct wl_data_source_listener data_source_listener = {
    .target = &target,
    .send = &send,
    .cancelled = &cancelled,
    .dnd_drop_performed = &dnd_drop_performed,
    .dnd_finished = &dnd_finished,
    .action = &action,
};

static void
primary_send(void *data,
             struct zwp_primary_selection_source_v1 *zwp_primary_selection_source_v1,
             const char *mime_type, int32_t fd)
{
    const struct primary *primary
        = zwp_primary_selection_source_v1_get_user_data(zwp_primary_selection_source_v1);

    assert(primary != NULL);
    assert(primary->text != NULL);

    size_t left = strlen(primary->text);
    size_t idx = 0;

    while (left > 0) {
        ssize_t ret = write(fd, &primary->text[idx], left);

        if (ret == -1 && errno != EINTR) {
            LOG_ERRNO("failed to write to clipboard");
            break;
        }

        left -= ret;
        idx += ret;
    }

    close(fd);
}

static void
primary_cancelled(void *data,
                  struct zwp_primary_selection_source_v1 *zwp_primary_selection_source_v1)
{
    struct primary *primary = zwp_primary_selection_source_v1_get_user_data(
        zwp_primary_selection_source_v1);
    //assert(primary->data_source == zwp_primary_selection_source_v1);

    zwp_primary_selection_source_v1_destroy(primary->data_source);
    primary->data_source = NULL;
    primary->serial = 0;

    free(primary->text);
    primary->text = NULL;
}

static const struct zwp_primary_selection_source_v1_listener primary_selection_source_listener = {
    .send = &primary_send,
    .cancelled = &primary_cancelled,
};

void
selection_to_clipboard(struct terminal *term, uint32_t serial)
{
    if (term->selection.clipboard.data_source != NULL) {
        /* Kill previous data source */
        struct clipboard *clipboard = &term->selection.clipboard;

        assert(clipboard->serial != 0);
        wl_data_device_set_selection(term->wl.data_device, NULL, clipboard->serial);
        wl_data_source_destroy(clipboard->data_source);
        free(clipboard->text);

        clipboard->data_source = NULL;
        clipboard->serial = 0;
    }

    struct clipboard *clipboard = &term->selection.clipboard;

    /* Get selection as a string */
    clipboard->text = extract_selection(term);

    clipboard->data_source
        = wl_data_device_manager_create_data_source(term->wl.data_device_manager);

    if (clipboard->data_source == NULL) {
        LOG_ERR("failed to create clipboard data source");
        return;
    }

    /* Configure source */
    wl_data_source_offer(clipboard->data_source, "text/plain;charset=utf-8");
    wl_data_source_add_listener(clipboard->data_source, &data_source_listener, term);
    wl_data_device_set_selection(term->wl.data_device, clipboard->data_source, serial);
    wl_data_source_set_user_data(clipboard->data_source, clipboard);

    /* Needed when sending the selection to other client */
    clipboard->serial = serial;
}

void
selection_from_clipboard(struct terminal *term, uint32_t serial)
{
    struct clipboard *clipboard = &term->selection.clipboard;
    if (clipboard->data_offer == NULL)
        return;

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        return;
    }

    int read_fd = fds[0];
    int write_fd = fds[1];

    /* Give write-end of pipe to other client */
    wl_data_offer_receive(
        clipboard->data_offer, "text/plain;charset=utf-8", write_fd);
    wl_display_roundtrip(term->wl.display);

    /* Don't keep our copy of the write-end open (or we'll never get EOF) */
    close(write_fd);

    if (term->bracketed_paste)
        vt_to_slave(term, "\033[200~", 6);

    /* Read until EOF */
    while (true) {
        char text[256];
        ssize_t amount = read(read_fd, text, sizeof(text));

        if (amount == -1) {
            LOG_ERRNO("failed to read clipboard data: %d", errno);
            break;
        } else if (amount == 0)
            break;

        vt_to_slave(term, text, amount);
    }

    if (term->bracketed_paste)
        vt_to_slave(term, "\033[201~", 6);

    close(read_fd);
}

void
selection_from_primary(struct terminal *term)
{
    struct primary *primary = &term->selection.primary;
    if (primary->data_offer == NULL)
        return;

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        return;
    }

    int read_fd = fds[0];
    int write_fd = fds[1];

    /* Give write-end of pipe to other client */
    zwp_primary_selection_offer_v1_receive(
        primary->data_offer, "text/plain;charset=utf-8", write_fd);
    wl_display_roundtrip(term->wl.display);

    /* Don't keep our copy of the write-end open (or we'll never get EOF) */
    close(write_fd);

    if (term->bracketed_paste)
        vt_to_slave(term, "\033[200~", 6);

    /* Read until EOF */
    while (true) {
        char text[256];
        ssize_t amount = read(read_fd, text, sizeof(text));

        if (amount == -1) {
            LOG_ERRNO("failed to read clipboard data: %d", errno);
            break;
        } else if (amount == 0)
            break;

        vt_to_slave(term, text, amount);
    }

    if (term->bracketed_paste)
        vt_to_slave(term, "\033[201~", 6);

    close(read_fd);
}

#if 0
static void
offer(void *data, struct wl_data_offer *wl_data_offer, const char *mime_type)
{
}

static void
source_actions(void *data, struct wl_data_offer *wl_data_offer,
                uint32_t source_actions)
{
}

static void
offer_action(void *data, struct wl_data_offer *wl_data_offer, uint32_t dnd_action)
{
}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer = &offer,
    .source_actions = &source_actions,
    .action = &offer_action,
};
#endif

static void
data_offer(void *data, struct wl_data_device *wl_data_device,
           struct wl_data_offer *id)
{
}

static void
enter(void *data, struct wl_data_device *wl_data_device, uint32_t serial,
      struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
      struct wl_data_offer *id)
{
}

static void
leave(void *data, struct wl_data_device *wl_data_device)
{
}

static void
motion(void *data, struct wl_data_device *wl_data_device, uint32_t time,
       wl_fixed_t x, wl_fixed_t y)
{
}

static void
drop(void *data, struct wl_data_device *wl_data_device)
{
}

static void
selection(void *data, struct wl_data_device *wl_data_device,
          struct wl_data_offer *id)
{
    /* Selection offer from other client */

    struct terminal *term = data;
    struct clipboard *clipboard = &term->selection.clipboard;

    if (clipboard->data_offer != NULL)
        wl_data_offer_destroy(clipboard->data_offer);

    clipboard->data_offer = id;
#if 0
    if (id != NULL)
        wl_data_offer_add_listener(id, &data_offer_listener, term);
#endif
}

const struct wl_data_device_listener data_device_listener = {
    .data_offer = &data_offer,
    .enter = &enter,
    .leave = &leave,
    .motion = &motion,
    .drop = &drop,
    .selection = &selection,
};

#if 0
static void
primary_offer(void *data,
              struct zwp_primary_selection_offer_v1 *zwp_primary_selection_offer,
              const char *mime_type)
{
}

static const struct zwp_primary_selection_offer_v1_listener primary_selection_offer_listener = {
    .offer = &primary_offer,
};
#endif

static void
primary_data_offer(void *data,
                   struct zwp_primary_selection_device_v1 *zwp_primary_selection_device,
                   struct zwp_primary_selection_offer_v1 *offer)
{
}

static void
primary_selection(void *data,
                  struct zwp_primary_selection_device_v1 *zwp_primary_selection_device,
                  struct zwp_primary_selection_offer_v1 *id)
{
    /* Selection offer from other client, for primary */

    struct terminal *term = data;
    struct primary *primary = &term->selection.primary;

    if (primary->data_offer != NULL)
        zwp_primary_selection_offer_v1_destroy(primary->data_offer);

    primary->data_offer = id;
#if 0
    if (id != NULL) {
        zwp_primary_selection_offer_v1_add_listener(
            id, &primary_selection_offer_listener, term);
    }
#endif
}

const struct zwp_primary_selection_device_v1_listener primary_selection_device_listener = {
    .data_offer = &primary_data_offer,
    .selection = &primary_selection,
};
