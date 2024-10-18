#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "async.h"
#include "config.h"
#include "key-binding.h"
#include "reaper.h"
#include "sixel.h"
#include "user-notification.h"
#include "vt.h"

extern bool fdm_ptmx(struct fdm *fdm, int fd, int events, void *data);

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
fdm_add(struct fdm *fdm, int fd, int events, fdm_fd_handler_t handler, void *data)
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
render_resize(
    struct terminal *term, int width, int height, uint8_t resize_options)
{
    return true;
}

void render_refresh(struct terminal *term) {}
void render_refresh_csd(struct terminal *term) {}
void render_refresh_title(struct terminal *term) {}
void render_refresh_app_id(struct terminal *term) {}
void render_refresh_icon(struct terminal *term) {}

bool
render_xcursor_is_valid(const struct seat *seat, const char *cursor)
{
    return true;
}

bool
render_xcursor_set(struct seat *seat, struct terminal *term, enum cursor_shape shape)
{
    return true;
}

enum cursor_shape
xcursor_for_csd_border(struct terminal *term, int x, int y)
{
    return CURSOR_SHAPE_LEFT_PTR;
}

struct wl_window *
wayl_win_init(struct terminal *term, const char *token)
{
    return NULL;
}

void wayl_win_destroy(struct wl_window *win) {}
void wayl_win_alpha_changed(struct wl_window *win) {}
bool wayl_win_set_urgent(struct wl_window *win) { return true; }
bool wayl_fractional_scaling(const struct wayland *wayl) { return true; }

pid_t
spawn(struct reaper *reaper, const char *cwd, char *const argv[],
      int stdin_fd, int stdout_fd, int stderr_fd,
      reaper_cb cb, void *cb_data, const char *xdg_activation_token)
{
    return 2;
}

pid_t
slave_spawn(
    int ptmx, int argc, const char *cwd, char *const *argv, char *const *envp,
    const env_var_list_t *extra_env_vars, const char *term_env,
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
extract_begin(enum selection_kind kind, bool strip_trailing_empty)
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

void ime_enable(struct seat *seat) {}
void ime_disable(struct seat *seat) {}
void ime_reset_preedit(struct seat *seat) {}

bool
notify_notify(struct terminal *term, struct notification *notif)
{
    return true;
}

void
notify_close(struct terminal *term, const char *id)
{
}

void
notify_free(struct terminal *term, struct notification *notif)
{
}

void
notify_icon_add(struct terminal *term, const char *id,
                const char *symbolic_name, const uint8_t *data,
                size_t data_sz)
{
}

void
notify_icon_del(struct terminal *term, const char *id)
{
}

void
notify_icon_free(struct notification_icon *icon)
{
}

void reaper_add(struct reaper *reaper, pid_t pid, reaper_cb cb, void *cb_data) {}
void reaper_del(struct reaper *reaper, pid_t pid) {}

void urls_reset(struct terminal *term) {}

void shm_unref(struct buffer *buf) {}
void shm_chain_free(struct buffer_chain *chain) {}

struct buffer_chain *
shm_chain_new(struct wl_shm *shm, bool scrollable, size_t pix_instances)
{
    return NULL;
}


void search_selection_cancelled(struct terminal *term) {}

void get_current_modifiers(const struct seat *seat,
                           xkb_mod_mask_t *effective,
                           xkb_mod_mask_t *consumed, uint32_t key,
                           bool filter_locked) {}

static struct key_binding_set kbd;
static bool kbd_initialized = false;

struct key_binding_set *
key_binding_for(
    struct key_binding_manager *mgr, const struct config *conf,
    const struct seat *seat)
{
    return &kbd;
}

void
key_binding_new_for_conf(
    struct key_binding_manager *mgr, const struct wayland *wayl,
    const struct config *conf)
{
    if (!kbd_initialized) {
        kbd_initialized = true;
        kbd = (struct key_binding_set){
            .key = tll_init(),
            .search = tll_init(),
            .url = tll_init(),
            .mouse = tll_init(),
            .selection_overrides = 0,
        };
    }
}

void
key_binding_unref(struct key_binding_manager *mgr, const struct config *conf)
{
}

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

    int lower_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (lower_fd < 0)
        return EXIT_FAILURE;

    int upper_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (upper_fd < 0) {
        close(lower_fd);
        return EXIT_FAILURE;
    }

    struct row **normal_rows = calloc(grid_row_count, sizeof(normal_rows[0]));
    struct row **alt_rows = calloc(grid_row_count, sizeof(alt_rows[0]));

    for (int i = 0; i < grid_row_count; i++) {
        normal_rows[i] = calloc(1, sizeof(*normal_rows[i]));
        normal_rows[i]->cells = calloc(col_count, sizeof(normal_rows[i]->cells[0]));
        alt_rows[i] = calloc(1, sizeof(*alt_rows[i]));
        alt_rows[i]->cells = calloc(col_count, sizeof(alt_rows[i]->cells[0]));
    }

    struct config conf = {
        .tweak = {
            .delayed_render_lower_ns = 500000,         /* 0.5ms */
            .delayed_render_upper_ns = 16666666 / 2,   /* half a frame period (60Hz) */
        },
    };

    struct wayland wayl = {
        .seats = tll_init(),
        .monitors = tll_init(),
        .terms = tll_init(),
    };

    struct terminal term = {
        .conf = &conf,
        .wl = &wayl,
        .grid = &term.normal,
        .normal = {
            .num_rows = grid_row_count,
            .num_cols = col_count,
            .rows = normal_rows,
            .cur_row = normal_rows[0],
        },
        .alt = {
            .num_rows = grid_row_count,
            .num_cols = col_count,
            .rows = alt_rows,
            .cur_row = alt_rows[0],
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
            .coords = {
                .start = {-1, -1},
                .end = {-1, -1},
            },
        },
        .delayed_render_timer = {
            .lower_fd = lower_fd,
            .upper_fd = upper_fd
        },
        .sixel = {
            .palette_size = SIXEL_MAX_COLORS,
            .max_width = SIXEL_MAX_WIDTH,
            .max_height = SIXEL_MAX_HEIGHT,
        },
    };

    tll_push_back(wayl.terms, &term);

    int ret = EXIT_FAILURE;

    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "error: %s: failed to stat: %s\n",
                    argv[i], strerror(errno));
            goto out;
        }

        uint8_t *data = malloc(st.st_size);
        if (data == NULL) {
            fprintf(stderr, "error: %s: failed to allocate buffer: %s\n",
                    argv[i], strerror(errno));
            goto out;
        }

        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "error: %s: failed to open: %s\n",
                    argv[i], strerror(errno));
            goto out;
        }

        ssize_t amount = read(fd, data, st.st_size);
        if (amount != st.st_size) {
            fprintf(stderr, "error: %s: failed to read: %s\n",
                    argv[i], strerror(errno));
            goto out;
        }

        close(fd);

#if defined(MEMFD_CREATE)
        int mem_fd = memfd_create("foot-pgo-ptmx", MFD_CLOEXEC);
#elif defined(__FreeBSD__)
        // memfd_create on FreeBSD 13 is SHM_ANON without sealing support
        int mem_fd = shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600);
#else
        char name[] = "/tmp/foot-pgo-ptmx-XXXXXX";
        int mem_fd = mkostemp(name, O_CLOEXEC);
        unlink(name);
#endif
        if (mem_fd < 0) {
            fprintf(stderr, "error: failed to create memory FD\n");
            goto out;
        }

        if (write(mem_fd, data, st.st_size) < 0) {
            fprintf(stderr, "error: failed to write memory FD\n");
            close(mem_fd);
            goto out;
        }

        free(data);

        term.ptmx = mem_fd;
        lseek(mem_fd, 0, SEEK_SET);

        printf("Feeding VT parser with %s (%lld bytes)\n",
               argv[i], (long long)st.st_size);

        while (lseek(mem_fd, 0, SEEK_CUR) < st.st_size) {
            if (!fdm_ptmx(NULL, -1, EPOLLIN, &term)) {
                fprintf(stderr, "error: fdm_ptmx() failed\n");
                close(mem_fd);
                goto out;
            }
        }
        close(mem_fd);
    }

    ret = EXIT_SUCCESS;

out:
    tll_free(wayl.terms);

    for (int i = 0; i < grid_row_count; i++) {
        if (normal_rows[i] != NULL)
            free(normal_rows[i]->cells);
        free(normal_rows[i]);

        if (alt_rows[i] != NULL)
            free(alt_rows[i]->cells);
        free(alt_rows[i]);
    }

    free(normal_rows);
    free(alt_rows);
    close(lower_fd);
    close(upper_fd);
    return ret;
}
