#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "async.h"
#include "user-notification.h"
#include "vt.h"

static void
usage(const char *prog_name)
{
    printf(
        "Usage: %s stimuli-file1 stimuli-file2 ... stimuli-fileN\n",
        prog_name);
}

enum async_write_status
async_write(int fd, const void *data, size_t len, size_t *idx)
{
    return ASYNC_WRITE_DONE;
}

bool
fdm_add(struct fdm *fdm, int fd, int events, fdm_handler_t handler, void *data)
{
    return true;
}

bool
fdm_del(struct fdm *fdm, int fd)
{
    return true;
}

bool
fdm_event_add(struct fdm *fdm, int fd, int events)
{
    return true;
}

bool
fdm_event_del(struct fdm *fdm, int fd, int events)
{
    return true;
}

bool
render_resize_force(struct terminal *term, int width, int height)
{
    return true;
}

void render_refresh(struct terminal *term) {}
void render_refresh_csd(struct terminal *term) {}
void render_refresh_title(struct terminal *term) {}

bool
render_xcursor_set(struct seat *seat, struct terminal *term, const char *xcursor)
{
    return true;
}

struct wl_window *
wayl_win_init(struct terminal *term)
{
    return NULL;
}

void wayl_win_destroy(struct wl_window *win) {}

bool
spawn(struct reaper *reaper, const char *cwd, char *const argv[],
      int stdin_fd, int stdout_fd, int stderr_fd)
{
    return true;
}

pid_t
slave_spawn(
    int ptmx, int argc, const char *cwd, char *const *argv, const char *term_env,
    const char *conf_shell, bool login_shell,
    const user_notifications_t *notifications)
{
    return 0;
}

int
render_worker_thread(void *_ctx)
{
    return 0;
}

struct extraction_context *
extract_begin(enum selection_kind kind)
{
    return NULL;
}

bool
extract_one(
    const struct terminal *term, const struct row *row, const struct cell *cell,
    int col, void *context)
{
    return true;
}

bool
extract_finish(struct extraction_context *context, char **text, size_t *len)
{
    return true;
}

void cmd_scrollback_up(struct terminal *term, int rows) {}
void cmd_scrollback_down(struct terminal *term, int rows) {}

int
main(int argc, const char *const *argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const int row_count = 67;
    const int col_count = 135;
    const int grid_row_count = 16384;

    struct row **rows = calloc(grid_row_count, sizeof(rows[0]));
    for (int i = 0; i < grid_row_count; i++) {
        rows[i] = calloc(1, sizeof(*rows[i]));
        rows[i]->cells = calloc(col_count, sizeof(rows[i]->cells[0]));
    }

    struct wayland wayl = {
        .seats = tll_init(),
        .monitors = tll_init(),
        .terms = tll_init(),
    };

    struct terminal term = {
        .wl = &wayl,
        .grid = &term.normal,
        .normal = {
            .num_rows = grid_row_count,
            .num_cols = col_count,
            .rows = rows,
            .cur_row = rows[0],
        },
        .alt = {
            .num_rows = grid_row_count,
            .num_cols = col_count,
            .rows = rows,
            .cur_row = rows[0],
        },
        .scale = 1,
        .width = col_count * 8,
        .height = row_count * 15,
        .cols = col_count,
        .rows = row_count,
        .cell_width = 8,
        .cell_height = 15,
        .scroll_region = {
            .start = 0,
            .end = row_count,
        },
        .selection = {
            .start = {-1, -1},
            .end = {-1, -1},
        },
    };

    tll_push_back(wayl.terms, &term);

    int ret = EXIT_FAILURE;

    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "error: %s: failed to stat: %s",
                    argv[i], strerror(errno));
            goto out;
        }

        uint8_t *data = malloc(st.st_size);
        if (data == NULL) {
            fprintf(stderr, "error: %s: failed to allocate buffer: %s",
                    argv[i], strerror(errno));
            goto out;
        }

        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "error: %s: failed to open: %s",
                    argv[i], strerror(errno));
            goto out;
        }

        ssize_t amount = read(fd, data, st.st_size);
        if (amount != st.st_size) {
            fprintf(stderr, "error: %s: failed to read: %s",
                    argv[i], strerror(errno));
            goto out;
        }

        close(fd);

        printf("Feeding VT parser with %s\n", argv[i]);
        vt_from_slave(&term, data, st.st_size);
        free(data);
    }

    ret = EXIT_SUCCESS;

out:
    tll_free(wayl.terms);

    for (int i = 0; i < grid_row_count; i++) {
        free(rows[i]->cells);
        free(rows[i]);
    }

    free(rows);
    return ret;
}
