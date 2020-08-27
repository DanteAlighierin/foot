#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <pwd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <fontconfig/fontconfig.h>

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "input.h"
#include "macros.h"
#include "tokenize.h"
#include "util.h"
#include "wayland.h"
#include "xmalloc.h"

static const uint32_t default_foreground = 0xdcdccc;
static const uint32_t default_background = 0x111111;

static const uint32_t default_regular[] = {
    0x222222,
    0xcc9393,
    0x7f9f7f,
    0xd0bf8f,
    0x6ca0a3,
    0xdc8cc3,
    0x93e0e3,
    0xdcdccc,
};

static const uint32_t default_bright[] = {
    0x666666,
    0xdca3a3,
    0xbfebbf,
    0xf0dfaf,
    0x8cd0d3,
    0xfcace3,
    0xb3ffff,
    0xffffff,
};

static const char *const binding_action_map[] = {
    [BIND_ACTION_NONE] = NULL,
    [BIND_ACTION_SCROLLBACK_UP] = "scrollback-up",
    [BIND_ACTION_SCROLLBACK_DOWN] = "scrollback-down",
    [BIND_ACTION_CLIPBOARD_COPY] = "clipboard-copy",
    [BIND_ACTION_CLIPBOARD_PASTE] = "clipboard-paste",
    [BIND_ACTION_PRIMARY_PASTE] = "primary-paste",
    [BIND_ACTION_SEARCH_START] = "search-start",
    [BIND_ACTION_FONT_SIZE_UP] = "font-increase",
    [BIND_ACTION_FONT_SIZE_DOWN] = "font-decrease",
    [BIND_ACTION_FONT_SIZE_RESET] = "font-reset",
    [BIND_ACTION_SPAWN_TERMINAL] = "spawn-terminal",
    [BIND_ACTION_MINIMIZE] = "minimize",
    [BIND_ACTION_MAXIMIZE] = "maximize",
    [BIND_ACTION_FULLSCREEN] = "fullscreen",
    [BIND_ACTION_PIPE_SCROLLBACK] = "pipe-scrollback",
    [BIND_ACTION_PIPE_VIEW] = "pipe-visible",
    [BIND_ACTION_PIPE_SELECTED] = "pipe-selected",

    /* Mouse-specific actions */
    [BIND_ACTION_SELECT_BEGIN] = "select-begin",
    [BIND_ACTION_SELECT_BEGIN_BLOCK] = "select-begin-block",
    [BIND_ACTION_SELECT_EXTEND] = "select-extend",
    [BIND_ACTION_SELECT_WORD] = "select-word",
    [BIND_ACTION_SELECT_WORD_WS] = "select-word-whitespace",
    [BIND_ACTION_SELECT_ROW] = "select-row",
};

static_assert(ALEN(binding_action_map) == BIND_ACTION_COUNT,
              "binding action map size mismatch");

static const char *const search_binding_action_map[] = {
    [BIND_ACTION_SEARCH_NONE] = NULL,
    [BIND_ACTION_SEARCH_CANCEL] = "cancel",
    [BIND_ACTION_SEARCH_COMMIT] = "commit",
    [BIND_ACTION_SEARCH_FIND_PREV] = "find-prev",
    [BIND_ACTION_SEARCH_FIND_NEXT] = "find-next",
    [BIND_ACTION_SEARCH_EDIT_LEFT] = "cursor-left",
    [BIND_ACTION_SEARCH_EDIT_LEFT_WORD] = "cursor-left-word",
    [BIND_ACTION_SEARCH_EDIT_RIGHT] = "cursor-right",
    [BIND_ACTION_SEARCH_EDIT_RIGHT_WORD] = "cursor-right-word",
    [BIND_ACTION_SEARCH_EDIT_HOME] = "cursor-home",
    [BIND_ACTION_SEARCH_EDIT_END] = "cursor-end",
    [BIND_ACTION_SEARCH_DELETE_PREV] = "delete-prev",
    [BIND_ACTION_SEARCH_DELETE_PREV_WORD] = "delete-prev-word",
    [BIND_ACTION_SEARCH_DELETE_NEXT] = "delete-next",
    [BIND_ACTION_SEARCH_DELETE_NEXT_WORD] = "delete-next-word",
    [BIND_ACTION_SEARCH_EXTEND_WORD] = "extend-to-word-boundary",
    [BIND_ACTION_SEARCH_EXTEND_WORD_WS] = "extend-to-next-whitespace",
};

static_assert(ALEN(search_binding_action_map) == BIND_ACTION_SEARCH_COUNT,
              "search binding action map size mismatch");

#define LOG_AND_NOTIFY_ERR(...)                                     \
    do {                                                            \
        LOG_ERR(__VA_ARGS__);                                       \
        char *text = xasprintf(__VA_ARGS__);                        \
        struct user_notification notif = {                          \
            .kind = USER_NOTIFICATION_ERROR,                        \
            .text = text,                                           \
        };                                                          \
        tll_push_back(conf->notifications, notif);                  \
    } while (0)

#define LOG_AND_NOTIFY_WARN(...)                            \
    do {                                                    \
        LOG_WARN(__VA_ARGS__);                              \
        char *text = xasprintf(__VA_ARGS__);                \
        struct user_notification notif = {                  \
            .kind = USER_NOTIFICATION_WARNING,              \
            .text = text,                                   \
        };                                                  \
        tll_push_back(conf->notifications, notif);          \
    } while (0)

#define LOG_AND_NOTIFY_ERRNO(...)                                       \
    do {                                                                \
        int _errno = errno;                                             \
        LOG_ERRNO(__VA_ARGS__);                                         \
        int len = snprintf(NULL, 0, __VA_ARGS__);                       \
        int errno_len = snprintf(NULL, 0, ": %s", strerror(_errno));    \
        char *text = xmalloc(len + errno_len + 1);                      \
        snprintf(text, len + errno_len + 1, __VA_ARGS__);               \
        snprintf(&text[len], errno_len + 1, ": %s", strerror(_errno));  \
        struct user_notification notif = {                              \
            .kind = USER_NOTIFICATION_ERROR,                            \
            .text = text,                                               \
        };                                                              \
        tll_push_back(conf->notifications, notif);                      \
    } while(0)

static char *
get_shell(void)
{
    const char *shell = getenv("SHELL");

    if (shell == NULL) {
        struct passwd *passwd = getpwuid(getuid());
        if (passwd == NULL) {
            LOG_ERRNO("failed to lookup user: falling back to 'sh'");
            shell = "sh";
        } else
            shell = passwd->pw_shell;
    }

    LOG_DBG("user's shell: %s", shell);
    return xstrdup(shell);
}

struct config_file {
    char *path;       /* Full, absolute, path */
    int fd;           /* FD of file, O_RDONLY */
};

struct path_component {
    const char *component;
    int fd;
};
typedef tll(struct path_component) path_components_t;

static void
path_component_add(path_components_t *components, const char *comp, int fd)
{
    assert(comp != NULL);
    assert(fd >= 0);

    struct path_component pc = {.component = comp, .fd = fd};
    tll_push_back(*components, pc);
}

static void
path_component_destroy(struct path_component *component)
{
    assert(component->fd >= 0);
    close(component->fd);
}

static void
path_components_destroy(path_components_t *components)
{
    tll_foreach(*components, it) {
        path_component_destroy(&it->item);
        tll_remove(*components, it);
    }
}

static struct config_file
path_components_to_config_file(const path_components_t *components)
{
    if (tll_length(*components) == 0)
        goto err;

    size_t len = 0;
    tll_foreach(*components, it)
        len += strlen(it->item.component) + 1;

    char *path = malloc(len);
    if (path == NULL)
        goto err;

    size_t idx = 0;
    tll_foreach(*components, it) {
        strcpy(&path[idx], it->item.component);
        idx += strlen(it->item.component);
        path[idx++] = '/';
    }
    path[idx - 1] = '\0';  /* Strip last ’/’ */

    int fd_copy = dup(tll_back(*components).fd);
    if (fd_copy < 0) {
        free(path);
        goto err;
    }

    return (struct config_file){.path = path, .fd = fd_copy};

err:
    return (struct config_file){.path = NULL, .fd = -1};
}

static const char *
get_user_home_dir(void)
{
    const struct passwd *passwd = getpwuid(getuid());
    if (passwd == NULL)
        return NULL;
    return passwd->pw_dir;
}

static bool
try_open_file(path_components_t *components, const char *name)
{
    int parent_fd = tll_back(*components).fd;

    struct stat st;
    if (fstatat(parent_fd, name, &st, 0) == 0 && S_ISREG(st.st_mode)) {
        int fd = openat(parent_fd, name, O_RDONLY);
        if (fd >= 0) {
            path_component_add(components, name, fd);
            return true;
        }
    }

    return false;
}

static struct config_file
open_config(struct config *conf)
{
    struct config_file ret = {.path = NULL, .fd = -1};
    bool log_deprecation = false;

    path_components_t components = tll_init();

    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char *user_home_dir = get_user_home_dir();
    char *xdg_config_dirs_copy = NULL;

    /* Use XDG_CONFIG_HOME, or ~/.config */
    if (xdg_config_home != NULL) {
        int fd = open(xdg_config_home, O_RDONLY);
        if (fd >= 0)
            path_component_add(&components, xdg_config_home, fd);
    } else if (user_home_dir != NULL) {
        int home_fd = open(user_home_dir, O_RDONLY);
        if (home_fd >= 0) {
            int config_fd = openat(home_fd, ".config", O_RDONLY);
            if (config_fd >= 0) {
                path_component_add(&components, user_home_dir, home_fd);
                path_component_add(&components, ".config", config_fd);
            } else
                close(home_fd);
        }
    }

    /* First look for foot/foot.ini */
    if (tll_length(components) > 0) {
        int foot_fd = openat(tll_back(components).fd, "foot", O_RDONLY);
        if (foot_fd >= 0) {
            path_component_add(&components, "foot", foot_fd);

            if (try_open_file(&components, "foot.ini"))
                goto done;

            struct path_component pc = tll_pop_back(components);
            path_component_destroy(&pc);
        }
    }

    /* Next try footrc */
    if (tll_length(components) > 0 && try_open_file(&components, "footrc")) {
        log_deprecation = true;
        goto done;
    }

    /* Finally, try foot/foot.ini in all XDG_CONFIG_DIRS */
    const char *xdg_config_dirs = getenv("XDG_CONFIG_DIRS");
    xdg_config_dirs_copy = xdg_config_dirs != NULL
        ? strdup(xdg_config_dirs) : NULL;

    if (xdg_config_dirs_copy != NULL) {
        for (char *save = NULL,
                 *xdg_dir = strtok_r(xdg_config_dirs_copy, ":", &save);
             xdg_dir != NULL;
             xdg_dir = strtok_r(NULL, ":", &save))
        {
            path_components_destroy(&components);

            int xdg_fd = open(xdg_dir, O_RDONLY);
            if (xdg_fd < 0)
                continue;

            int foot_fd = openat(xdg_fd, "foot", O_RDONLY);
            if (foot_fd < 0) {
                close(xdg_fd);
                continue;
            }

            assert(tll_length(components) == 0);
            path_component_add(&components, xdg_dir, xdg_fd);
            path_component_add(&components, "foot", foot_fd);

            if (try_open_file(&components, "foot.ini"))
                goto done;
        }
    }

out:
    path_components_destroy(&components);
    free(xdg_config_dirs_copy);
    return ret;

done:
    assert(tll_length(components) > 0);
    ret = path_components_to_config_file(&components);

    if (log_deprecation && ret.path != NULL) {
        LOG_WARN("deprecated: configuration in $XDG_CONFIG_HOME/footrc, "
                 "use $XDG_CONFIG_HOME/foot/foot.ini instead");

        char *text = xstrdup(
            "configuration in \033[31m$XDG_CONFIG_HOME/footrc\033[39m or "
            "\033[31m~/.config/footrc\033[39m, "
            "use \033[32m$XDG_CONFIG_HOME/foot/foot.ini\033[39m or "
            "\033[32m~/.config/foot/foot.ini\033[39m instead");

        struct user_notification deprecation = {
            .kind = USER_NOTIFICATION_DEPRECATED,
            .text = text,
        };
        tll_push_back(conf->notifications, deprecation);
    }
    goto out;
}

static bool
str_to_bool(const char *s)
{
    return strcasecmp(s, "on") == 0 ||
        strcasecmp(s, "true") == 0 ||
        strcasecmp(s, "yes") == 0 ||
        strtoul(s, NULL, 0) > 0;
}

static bool
str_to_ulong(const char *s, int base, unsigned long *res)
{
    if (s == NULL)
        return false;

    errno = 0;
    char *end = NULL;

    *res = strtoul(s, &end, base);
    return errno == 0 && *end == '\0';
}

static bool
str_to_double(const char *s, double *res)
{
    if (s == NULL)
        return false;

    errno = 0;
    char *end = NULL;

    *res = strtod(s, &end);
    return errno == 0 && *end == '\0';
}

static bool
str_to_color(const char *s, uint32_t *color, bool allow_alpha, const char *path, int lineno,
             const char *section, const char *key)
{
    unsigned long value;
    if (!str_to_ulong(s, 16, &value)) {
        LOG_ERRNO("%s:%d: [%s]: %s: invalid color: %s", path, lineno, section, key, s);
        return false;
    }

    if (!allow_alpha && (value & 0xff000000) != 0) {
        LOG_ERR("%s:%d: [%s]: %s: color value must not have an alpha component: %s",
                path, lineno, section, key, s);
        return false;
    }

    *color = value;
    return true;
}

static bool
parse_section_main(const char *key, const char *value, struct config *conf,
                   const char *path, unsigned lineno)
{
    if (strcmp(key, "term") == 0) {
        free(conf->term);
        conf->term = xstrdup(value);
    }

    else if (strcmp(key, "shell") == 0) {
        free(conf->shell);
        conf->shell = xstrdup(value);
    }

    else if (strcmp(key, "login-shell") == 0) {
        conf->login_shell = str_to_bool(value);
    }

    else if (strcmp(key, "title") == 0) {
        free(conf->title);
        conf->title = xstrdup(value);
    }

    else if (strcmp(key, "app-id") == 0) {
        free(conf->app_id);
        conf->app_id = xstrdup(value);
    }

    else if (strcmp(key, "geometry") == 0) {
        unsigned width, height;
        if (sscanf(value, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
            LOG_AND_NOTIFY_ERR(
                "%s: %d: [default]: geometry: expected WIDTHxHEIGHT, "
                "where both are positive integers, got '%s'",
                path, lineno, value);
            return false;
        }

        conf->width = width;
        conf->height = height;
    }

    else if (strcmp(key, "pad") == 0) {
        unsigned x, y;
        if (sscanf(value, "%ux%u", &x, &y) != 2) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [default]: pad: expected PAD_XxPAD_Y, "
                "where both are positive integers, got '%s'",
                path, lineno, value);
            return false;
        }

        conf->pad_x = x;
        conf->pad_y = y;
    }

    else if (strcmp(key, "initial-window-mode") == 0) {
        if (strcmp(value, "windowed") == 0)
            conf->startup_mode = STARTUP_WINDOWED;
        else if (strcmp(value, "maximized") == 0)
            conf->startup_mode = STARTUP_MAXIMIZED;
        else if (strcmp(value, "fullscreen") == 0)
            conf->startup_mode = STARTUP_FULLSCREEN;
        else {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [default]: initial-window-mode: expected either "
                "'windowed', 'maximized' or 'fullscreen'",
                path, lineno);
            return false;
        }
    }

    else if (strcmp(key, "font") == 0) {
        char *copy = xstrdup(value);
        for (const char *font = strtok(copy, ","); font != NULL; font = strtok(NULL, ",")) {
            /* Trim spaces, strictly speaking not necessary, but looks nice :) */
            while (*font != '\0' && isspace(*font))
                font++;
            if (*font != '\0')
                tll_push_back(conf->fonts, config_font_parse(font));
        }
        free(copy);
    }

    else if (strcmp(key, "workers") == 0) {
        unsigned long count;
        if (!str_to_ulong(value, 10, &count)) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [default]: workers: expected an integer, got '%s'",
                path, lineno, value);
            return false;
        }
        conf->render_worker_count = count;
    }

    else if (strcmp(key, "scrollback") == 0) {
        LOG_WARN("deprecated: [default]: scrollback: use 'scrollback.lines' instead'");

        const char *fmt = "%s:%d: \033[1mdefault.scrollback\033[21m, use \033[1mscrollback.lines\033[21m instead";
        char *text = xasprintf(fmt, path, lineno);

        struct user_notification deprecation = {
            .kind = USER_NOTIFICATION_DEPRECATED,
            .text = text,
        };
        tll_push_back(conf->notifications, deprecation);

        unsigned long lines;
        if (!str_to_ulong(value, 10, &lines)) {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [default]: scrollback: expected an integer, got '%s'",
                path, lineno, value);
            return false;
        }
        conf->scrollback.lines = lines;
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%u: [default]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_scrollback(const char *key, const char *value, struct config *conf,
                         const char *path, unsigned lineno)
{
    if (strcmp(key, "lines") == 0) {
        unsigned long lines;
        if (!str_to_ulong(value, 10, &lines)) {
            LOG_AND_NOTIFY_ERR("%s:%d: [scrollback]: lines: expected an integer, got '%s'", path, lineno, value);
            return false;
        }
        conf->scrollback.lines = lines;
    }

    else if (strcmp(key, "indicator-position") == 0) {
        if (strcmp(value, "none") == 0)
            conf->scrollback.indicator.position = SCROLLBACK_INDICATOR_POSITION_NONE;
        else if (strcmp(value, "fixed") == 0)
            conf->scrollback.indicator.position = SCROLLBACK_INDICATOR_POSITION_FIXED;
        else if (strcmp(value, "relative") == 0)
            conf->scrollback.indicator.position = SCROLLBACK_INDICATOR_POSITION_RELATIVE;
        else {
            LOG_AND_NOTIFY_ERR("%s:%d: [scrollback]: indicator-position must be one of "
                    "'none', 'fixed' or 'relative'",
                    path, lineno);
            return false;
        }
    }

    else if (strcmp(key, "indicator-format") == 0) {
        if (strcmp(value, "percentage") == 0) {
            conf->scrollback.indicator.format
                = SCROLLBACK_INDICATOR_FORMAT_PERCENTAGE;
        } else if (strcmp(value, "line") == 0) {
            conf->scrollback.indicator.format
                = SCROLLBACK_INDICATOR_FORMAT_LINENO;
        } else {
            free(conf->scrollback.indicator.text);
            conf->scrollback.indicator.text = NULL;

            size_t len = mbstowcs(NULL, value, 0);
            if (len == (size_t)-1) {
                LOG_AND_NOTIFY_ERRNO(
                    "%s:%d: [scrollback]: indicator-format: "
                    "invalid value: %s", path, lineno, value);
                return false;
            }

            conf->scrollback.indicator.text = xcalloc(len + 1, sizeof(wchar_t));
            mbstowcs(conf->scrollback.indicator.text, value, len);
        }
    }

    else if (strcmp(key, "multiplier") == 0) {
        double multiplier;
        if (!str_to_double(value, &multiplier)) {
            LOG_AND_NOTIFY_ERR("%s:%d: [scrollback]: multiplier: "
                               "invalid value: %s", path, lineno, value);
            return false;
        }

        conf->scrollback.multiplier = multiplier;
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%u: [scrollback]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_colors(const char *key, const char *value, struct config *conf,
                     const char *path, unsigned lineno)
{
    uint32_t *color = NULL;

    if (strcmp(key, "foreground") == 0)      color = &conf->colors.fg;
    else if (strcmp(key, "background") == 0) color = &conf->colors.bg;
    else if (strcmp(key, "regular0") == 0)   color = &conf->colors.regular[0];
    else if (strcmp(key, "regular1") == 0)   color = &conf->colors.regular[1];
    else if (strcmp(key, "regular2") == 0)   color = &conf->colors.regular[2];
    else if (strcmp(key, "regular3") == 0)   color = &conf->colors.regular[3];
    else if (strcmp(key, "regular4") == 0)   color = &conf->colors.regular[4];
    else if (strcmp(key, "regular5") == 0)   color = &conf->colors.regular[5];
    else if (strcmp(key, "regular6") == 0)   color = &conf->colors.regular[6];
    else if (strcmp(key, "regular7") == 0)   color = &conf->colors.regular[7];
    else if (strcmp(key, "bright0") == 0)    color = &conf->colors.bright[0];
    else if (strcmp(key, "bright1") == 0)    color = &conf->colors.bright[1];
    else if (strcmp(key, "bright2") == 0)    color = &conf->colors.bright[2];
    else if (strcmp(key, "bright3") == 0)    color = &conf->colors.bright[3];
    else if (strcmp(key, "bright4") == 0)    color = &conf->colors.bright[4];
    else if (strcmp(key, "bright5") == 0)    color = &conf->colors.bright[5];
    else if (strcmp(key, "bright6") == 0)    color = &conf->colors.bright[6];
    else if (strcmp(key, "bright7") == 0)    color = &conf->colors.bright[7];
    else if (strcmp(key, "selection-foreground") == 0) color = &conf->colors.selection_fg;
    else if (strcmp(key, "selection-background") == 0) color = &conf->colors.selection_bg;
    else if (strcmp(key, "alpha") == 0) {
        double alpha;
        if (!str_to_double(value, &alpha) || alpha < 0. || alpha > 1.) {
            LOG_AND_NOTIFY_ERR("%s: %d: [colors]: alpha: expected a value in the range 0.0-1.0",
                    path, lineno);
            return false;
        }

        conf->colors.alpha = alpha * 65535.;
        return true;
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%d: [colors]: %s: invalid key", path, lineno, key);
        return false;
    }

    uint32_t color_value;
    if (!str_to_color(value, &color_value, false, path, lineno, "colors", key))
        return false;

    *color = color_value;
    return true;
}

static bool
parse_section_cursor(const char *key, const char *value, struct config *conf,
                     const char *path, unsigned lineno)
{
    if (strcmp(key, "style") == 0) {
        if (strcmp(value, "block") == 0)
            conf->cursor.style = CURSOR_BLOCK;
        else if (strcmp(value, "bar") == 0)
            conf->cursor.style = CURSOR_BAR;
        else if (strcmp(value, "underline") == 0)
            conf->cursor.style = CURSOR_UNDERLINE;

        else {
            LOG_AND_NOTIFY_ERR("%s:%d: invalid 'style': %s", path, lineno, value);
            return false;
        }
    }

    else if (strcmp(key, "blink") == 0)
        conf->cursor.blink = str_to_bool(value);

    else if (strcmp(key, "color") == 0) {
        char *value_copy = xstrdup(value);
        const char *text = strtok(value_copy, " ");
        const char *cursor = strtok(NULL, " ");

        uint32_t text_color, cursor_color;
        if (text == NULL || cursor == NULL ||
            !str_to_color(text, &text_color, false, path, lineno, "cursor", "color") ||
            !str_to_color(cursor, &cursor_color, false, path, lineno, "cursor", "color"))
        {
            LOG_AND_NOTIFY_ERR("%s:%d: invalid cursor colors: %s", path, lineno, value);
            free(value_copy);
            return false;
        }

        conf->cursor.color.text = 1u << 31 | text_color;
        conf->cursor.color.cursor = 1u << 31 | cursor_color;
        free(value_copy);
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%d: [cursor]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_mouse(const char *key, const char *value, struct config *conf,
                    const char *path, unsigned lineno)
{
    if (strcmp(key, "hide-when-typing") == 0)
        conf->mouse.hide_when_typing = str_to_bool(value);

    else {
        LOG_AND_NOTIFY_ERR("%s:%d: [mouse]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_csd(const char *key, const char *value, struct config *conf,
                     const char *path, unsigned lineno)
{
    if (strcmp(key, "preferred") == 0) {
        if (strcmp(value, "server") == 0)
            conf->csd.preferred = CONF_CSD_PREFER_SERVER;
        else if (strcmp(value, "client") == 0)
            conf->csd.preferred = CONF_CSD_PREFER_CLIENT;
        else {
            LOG_AND_NOTIFY_ERR("%s:%d: csd.preferred: expected either 'server' or 'client'", path, lineno);
            return false;
        }
    }

    else if (strcmp(key, "color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, path, lineno, "csd", "color")) {
            LOG_AND_NOTIFY_ERR("%s:%d: invalid titlebar-color: %s", path, lineno, value);
            return false;
        }

        conf->csd.color.title_set = true;
        conf->csd.color.title = color;
    }

    else if (strcmp(key, "size") == 0) {
        unsigned long pixels;
        if (!str_to_ulong(value, 10, &pixels)) {
            LOG_AND_NOTIFY_ERR("%s:%d: expected an integer, got '%s'", path, lineno, value);
            return false;
        }

        conf->csd.title_height = pixels;
    }

    else if (strcmp(key, "button-width") == 0) {
        unsigned long pixels;
        if (!str_to_ulong(value, 10, &pixels)) {
            LOG_AND_NOTIFY_ERR("%s:%d: expected an integer, got '%s'", path, lineno, value);
            return false;
        }

        conf->csd.button_width = pixels;
    }

    else if (strcmp(key, "button-minimize-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, path, lineno, "csd", "button-minimize-color")) {
            LOG_AND_NOTIFY_ERR("%s:%d: invalid button-minimize-color: %s", path, lineno, value);
            return false;
        }

        conf->csd.color.minimize_set = true;
        conf->csd.color.minimize = color;
    }

    else if (strcmp(key, "button-maximize-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, path, lineno, "csd", "button-maximize-color")) {
            LOG_AND_NOTIFY_ERR("%s:%d: invalid button-maximize-color: %s", path, lineno, value);
            return false;
        }

        conf->csd.color.maximize_set = true;
        conf->csd.color.maximize = color;
    }

    else if (strcmp(key, "button-close-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, path, lineno, "csd", "button-close-color")) {
            LOG_AND_NOTIFY_ERR("%s:%d: invalid button-close-color: %s", path, lineno, value);
            return false;
        }

        conf->csd.color.close_set = true;
        conf->csd.color.close = color;
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%u: [csd]: %s: invalid action",
                           path, lineno, key);
        return false;
    }

    return true;
}

/* Struct that holds temporary key/mouse binding parsed data */
struct key_combo {
    char *text;          /* Raw text, e.g. "Control+Shift+V" */
    struct config_key_modifiers modifiers;
    union {
        xkb_keysym_t sym;    /* Key converted to an XKB symbol, e.g. XKB_KEY_V */
        struct {
            int button;
            int count;
        } m;
    };
};
typedef tll(struct key_combo) key_combo_list_t;

static void
free_key_combo_list(key_combo_list_t *key_combos)
{
    tll_foreach(*key_combos, it)
        free(it->item.text);
    tll_free(*key_combos);
}

static bool
parse_modifiers(struct config *conf, const char *text, size_t len,
                struct config_key_modifiers *modifiers, const char *path, unsigned lineno)
{
    bool ret = false;

    *modifiers = (struct config_key_modifiers){0};
    char *copy = xstrndup(text, len);

    for (char *tok_ctx = NULL, *key = strtok_r(copy, "+", &tok_ctx);
         key != NULL;
         key = strtok_r(NULL, "+", &tok_ctx))
    {
        if (strcmp(key, XKB_MOD_NAME_SHIFT) == 0)
            modifiers->shift = true;
        else if (strcmp(key, XKB_MOD_NAME_CTRL) == 0)
            modifiers->ctrl = true;
        else if (strcmp(key, XKB_MOD_NAME_ALT) == 0)
            modifiers->alt = true;
        else if (strcmp(key, XKB_MOD_NAME_LOGO) == 0)
            modifiers->meta = true;
        else {
            LOG_AND_NOTIFY_ERR("%s:%d: %s: not a valid modifier name",
                               path, lineno, key);
            goto out;
        }
    }

    ret = true;

out:
    free(copy);
    return ret;
}

static bool
parse_key_combos(struct config *conf, const char *combos, key_combo_list_t *key_combos,
                 const char *path, unsigned lineno)
{
    assert(tll_length(*key_combos) == 0);

    char *copy = xstrdup(combos);

    for (char *tok_ctx = NULL, *combo = strtok_r(copy, " ", &tok_ctx);
         combo != NULL;
         combo = strtok_r(NULL, " ", &tok_ctx))
    {
        struct config_key_modifiers modifiers = {0};
        const char *key = strrchr(combo, '+');

        if (key == NULL) {
            /* No modifiers */
            key = combo;
        } else {
            if (!parse_modifiers(conf, combo, key - combo, &modifiers, path, lineno))
                goto err;
            key++;  /* Skip past the '+' */
        }

        /* Translate key name to symbol */
        xkb_keysym_t sym = xkb_keysym_from_name(key, 0);
        if (sym == XKB_KEY_NoSymbol) {
            LOG_AND_NOTIFY_ERR("%s:%d: %s: key is not a valid XKB key name",
                               path, lineno, key);
            goto err;
        }

        tll_push_back(
            *key_combos,
            ((struct key_combo){.text = xstrdup(combo), .modifiers = modifiers, .sym = sym}));
    }

    free(copy);
    return true;

err:
    tll_foreach(*key_combos, it)
        free(it->item.text);
    tll_free(*key_combos);
    free(copy);
    return false;
}

static bool
has_key_binding_collisions(struct config *conf, const key_combo_list_t *key_combos,
                           const char *path, unsigned lineno)
{
    tll_foreach(conf->bindings.key, it) {
        tll_foreach(*key_combos, it2) {
            const struct config_key_modifiers *mods1 = &it->item.modifiers;
            const struct config_key_modifiers *mods2 = &it2->item.modifiers;

            if (memcmp(mods1, mods2, sizeof(*mods1)) == 0 &&
                it->item.sym == it2->item.sym)
            {
                bool has_pipe = it->item.pipe.cmd != NULL;
                LOG_AND_NOTIFY_ERR("%s:%d: %s already mapped to '%s%s%s%s'",
                                   path, lineno, it2->item.text,
                                   binding_action_map[it->item.action],
                                   has_pipe ? " [" : "",
                                   has_pipe ? it->item.pipe.cmd : "",
                                   has_pipe ? "]" : "");
                return true;
            }
        }
    }

    return false;
}

static bool
has_search_binding_collisions(struct config *conf, const key_combo_list_t *key_combos,
                              const char *path, unsigned lineno)
{
    tll_foreach(conf->bindings.search, it) {
        tll_foreach(*key_combos, it2) {
            const struct config_key_modifiers *mods1 = &it->item.modifiers;
            const struct config_key_modifiers *mods2 = &it2->item.modifiers;

            if (memcmp(mods1, mods2, sizeof(*mods1)) == 0 &&
                it->item.sym == it2->item.sym)
            {
                LOG_AND_NOTIFY_ERR("%s:%d: %s already mapped to '%s'",
                                   path, lineno, it2->item.text,
                                   search_binding_action_map[it->item.action]);
                return true;
            }
        }
    }

    return false;
}

static int
argv_compare(char *const *argv1, char *const *argv2)
{
    assert(argv1 != NULL);
    assert(argv2 != NULL);

    for (size_t i = 0; ; i++) {
        if (argv1[i] == NULL && argv2[i] == NULL)
            return 0;
        if (argv1[i] == NULL)
            return -1;
        if (argv2[i] == NULL)
            return 1;

        int ret = strcmp(argv1[i], argv2[i]);
        if (ret != 0)
            return ret;
    }

    assert(false);
    return 1;
}

static bool
parse_section_key_bindings(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno)
{
    char *pipe_cmd = NULL;
    char **pipe_argv = NULL;
    size_t pipe_len = 0;

    if (value[0] == '[') {
        const char *pipe_cmd_end = strrchr(value, ']');
        if (pipe_cmd_end == NULL) {
            LOG_AND_NOTIFY_ERR("%s:%d: unclosed '['", path, lineno);
            return false;
        }

        pipe_len = pipe_cmd_end - value - 1;
        pipe_cmd = xstrndup(&value[1], pipe_len);

        if (!tokenize_cmdline(pipe_cmd, &pipe_argv)) {
            LOG_AND_NOTIFY_ERR("%s:%d: syntax error in command line", path, lineno);
            free(pipe_cmd);
            return false;
        }

        value = pipe_cmd_end + 1;
        while (isspace(*value))
            value++;
    }

    for (enum bind_action_normal action = 0;
         action < BIND_ACTION_KEY_COUNT;
         action++)
    {
        if (binding_action_map[action] == NULL)
            continue;

        if (strcmp(key, binding_action_map[action]) != 0)
            continue;

        /* Unset binding */
        if (strcasecmp(value, "none") == 0) {
            tll_foreach(conf->bindings.key, it) {
                if (it->item.action == action) {
                    if (it->item.pipe.master_copy) {
                        free(it->item.pipe.cmd);
                        free(it->item.pipe.argv);
                    }
                    tll_remove(conf->bindings.key, it);
                }
            }
            free(pipe_argv);
            free(pipe_cmd);
            return true;
        }

        key_combo_list_t key_combos = tll_init();
        if (!parse_key_combos(conf, value, &key_combos, path, lineno) ||
            has_key_binding_collisions(conf, &key_combos, path, lineno))
        {
            free(pipe_argv);
            free(pipe_cmd);
            free_key_combo_list(&key_combos);
            return false;
        }

        /* Remove existing bindings for this action+pipe */
        tll_foreach(conf->bindings.key, it) {
            if (it->item.action == action &&
                ((it->item.pipe.argv == NULL && pipe_argv == NULL) ||
                 (it->item.pipe.argv != NULL && pipe_argv != NULL &&
                  argv_compare(it->item.pipe.argv, pipe_argv) == 0)))
            {

                if (it->item.pipe.master_copy) {
                    free(it->item.pipe.cmd);
                    free(it->item.pipe.argv);
                }
                tll_remove(conf->bindings.key, it);
            }
        }

        /* Emit key bindings */
        bool first = true;
        tll_foreach(key_combos, it) {
            struct config_key_binding_normal binding = {
                .action = action,
                .modifiers = it->item.modifiers,
                .sym = it->item.sym,
                .pipe = {
                    .cmd = pipe_cmd,
                    .argv = pipe_argv,
                    .master_copy = first,
                },
            };

            tll_push_back(conf->bindings.key, binding);
            first = false;
        }

        free_key_combo_list(&key_combos);
        return true;
    }

    LOG_AND_NOTIFY_ERR("%s:%u: [key-bindings]: %s: invalid action",
                       path, lineno, key);
    return false;

}

static bool
parse_section_search_bindings(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno)
{
    for (enum bind_action_search action = 0;
         action < BIND_ACTION_SEARCH_COUNT;
         action++)
    {
        if (search_binding_action_map[action] == NULL)
            continue;

        if (strcmp(key, search_binding_action_map[action]) != 0)
            continue;

        /* Unset binding */
        if (strcasecmp(value, "none") == 0) {
            tll_foreach(conf->bindings.search, it) {
                if (it->item.action == action)
                    tll_remove(conf->bindings.search, it);
            }
            return true;
        }

        key_combo_list_t key_combos = tll_init();
        if (!parse_key_combos(conf, value, &key_combos, path, lineno) ||
            has_search_binding_collisions(conf, &key_combos, path, lineno))
        {
            free_key_combo_list(&key_combos);
            return false;
        }

        /* Remove existing bindings for this action */
        tll_foreach(conf->bindings.search, it) {
            if (it->item.action == action)
                tll_remove(conf->bindings.search, it);
        }

        /* Emit key bindings */
        tll_foreach(key_combos, it) {
            struct config_key_binding_search binding = {
                .action = action,
                .modifiers = it->item.modifiers,
                .sym = it->item.sym,
            };
            tll_push_back(conf->bindings.search, binding);
        }

        free_key_combo_list(&key_combos);
        return true;
    }

    LOG_AND_NOTIFY_ERR("%s:%u: [search-bindings]: %s: invalid key", path, lineno, key);
    return false;

}

static bool
parse_mouse_combos(struct config *conf, const char *combos, key_combo_list_t *key_combos,
                   const char *path, unsigned lineno)
{
    assert(tll_length(*key_combos) == 0);

    char *copy = xstrdup(combos);

    for (char *tok_ctx = NULL, *combo = strtok_r(copy, " ", &tok_ctx);
         combo != NULL;
         combo = strtok_r(NULL, " ", &tok_ctx))
    {
        struct config_key_modifiers modifiers = {0};
        char *key = strrchr(combo, '+');

        if (key == NULL) {
            /* No modifiers */
            key = combo;
        } else {
            *key = '\0';
            if (!parse_modifiers(conf, combo, key - combo, &modifiers, path, lineno))
                goto err;
            if (modifiers.shift) {
                LOG_AND_NOTIFY_ERR("%s:%d: Shift cannot be used in mosue bindings",
                                   path, lineno);
                goto err;
            }
            key++;  /* Skip past the '+' */
        }

        size_t count = 0;
        {
            char *_count = strrchr(key, '-');
            if (_count != NULL) {
                *_count = '\0';
                _count++;

                errno = 0;
                char *end;
                unsigned long value = strtoul(_count, &end, 10);
                if (_count[0] == '\0' || *end != '\0' || errno != 0) {
                    if (errno != 0)
                        LOG_AND_NOTIFY_ERRNO(
                            "%s:%d: %s: invalid click count", path, lineno, _count);
                    else
                        LOG_AND_NOTIFY_ERR(
                            "%s:%d: %s: invalid click count", path, lineno, _count);
                    goto err;
                }
                count = value;
            }
        }

        static const struct {
            const char *name;
            int code;
        } map[] = {
            {"BTN_LEFT", BTN_LEFT},
            {"BTN_RIGHT", BTN_RIGHT},
            {"BTN_MIDDLE", BTN_MIDDLE},
            {"BTN_SIDE", BTN_SIDE},
            {"BTN_EXTRA", BTN_EXTRA},
            {"BTN_FORWARD", BTN_FORWARD},
            {"BTN_BACK", BTN_BACK},
            {"BTN_TASK", BTN_TASK},
        };

        int button = 0;
        for (size_t i = 0; i < ALEN(map); i++) {
            if (strcmp(key, map[i].name) == 0) {
                button = map[i].code;
                break;
            }
        }

        if (button == 0) {
            LOG_AND_NOTIFY_ERR("%s:%d: %s: invalid mouse button name", path, lineno, key);
            goto err;
        }

        struct key_combo new = {
            .text = xstrdup(combo),
            .modifiers = modifiers,
            .m = {
                .button = button,
                .count = count,
            },
        };
        tll_push_back(*key_combos, new);
    }

    free(copy);
    return true;

err:
    tll_foreach(*key_combos, it)
        free(it->item.text);
    tll_free(*key_combos);
    free(copy);
    return false;
}

static bool
has_mouse_binding_collisions(struct config *conf, const key_combo_list_t *key_combos,
                             const char *path, unsigned lineno)
{
    tll_foreach(conf->bindings.mouse, it) {
        tll_foreach(*key_combos, it2) {
            const struct config_key_modifiers *mods1 = &it->item.modifiers;
            const struct config_key_modifiers *mods2 = &it2->item.modifiers;

            if (memcmp(mods1, mods2, sizeof(*mods1)) == 0 &&
                it->item.button == it2->item.m.button &&
                it->item.count == it2->item.m.count)
            {
                LOG_AND_NOTIFY_ERR("%s:%d: %s already mapped to '%s'",
                                   path, lineno, it2->item.text,
                                   binding_action_map[it->item.action]);
                return true;
            }
        }
    }

    return false;
}


static bool
parse_section_mouse_bindings(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno)
{
    for (enum bind_action_normal action = 0;
         action < BIND_ACTION_COUNT;
         action++)
    {
        if (binding_action_map[action] == NULL)
            continue;

        if (strcmp(key, binding_action_map[action]) != 0)
            continue;

        /* Unset binding */
        if (strcasecmp(value, "none") == 0) {
            tll_foreach(conf->bindings.mouse, it) {
                if (it->item.action == action)
                    tll_remove(conf->bindings.mouse, it);
            }
            return true;
        }

        key_combo_list_t key_combos = tll_init();
        if (!parse_mouse_combos(conf, value, &key_combos, path, lineno) ||
            has_mouse_binding_collisions(conf, &key_combos, path, lineno))
        {
            free_key_combo_list(&key_combos);
            return false;
        }

        /* Remove existing bindings for this action */
        tll_foreach(conf->bindings.mouse, it) {
            if (it->item.action == action) {
                tll_remove(conf->bindings.mouse, it);
            }
        }

        /* Emit mouse bindings */
        tll_foreach(key_combos, it) {
            struct config_mouse_binding binding = {
                .action = action,
                .modifiers = it->item.modifiers,
                .button = it->item.m.button,
                .count = it->item.m.count,
            };
            tll_push_back(conf->bindings.mouse, binding);
        }

        free_key_combo_list(&key_combos);
        return true;
    }

    LOG_AND_NOTIFY_ERR("%s:%u: [mouse-bindings]: %s: invalid key", path, lineno, key);
    return false;
}

static bool
parse_section_tweak(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno)
{
    if (strcmp(key, "render-timer") == 0) {
        if (strcmp(value, "none") == 0) {
            conf->tweak.render_timer_osd = false;
            conf->tweak.render_timer_log = false;
        } else if (strcmp(value, "osd") == 0) {
            conf->tweak.render_timer_osd = true;
            conf->tweak.render_timer_log = false;
        } else if (strcmp(value, "log") == 0) {
            conf->tweak.render_timer_osd = false;
            conf->tweak.render_timer_log = true;
        } else if (strcmp(value, "both") == 0) {
            conf->tweak.render_timer_osd = true;
            conf->tweak.render_timer_log = true;
        } else {
            LOG_AND_NOTIFY_ERR(
                "%s:%d: [tweak]: %s: invalid 'render-timer' value, "
                "expected one of 'none', 'osd', 'log' or 'both'",
                path, lineno, value);
            return false;
        }
    }

    else if (strcmp(key, "delayed-render-lower") == 0) {
        unsigned long ns;
        if (!str_to_ulong(value, 10, &ns)) {
            LOG_AND_NOTIFY_ERR("%s:%d: expected an integer, got '%s'", path, lineno, value);
            return false;
        }

        if (ns > 16666666) {
            LOG_AND_NOTIFY_ERR("%s:%d: timeout must not exceed 16ms", path, lineno);
            return false;
        }

        conf->tweak.delayed_render_lower_ns = ns;
        LOG_WARN("tweak: delayed-render-lower=%lu", ns);
    }

    else if (strcmp(key, "delayed-render-upper") == 0) {
        unsigned long ns;
        if (!str_to_ulong(value, 10, &ns)) {
            LOG_AND_NOTIFY_ERR("%s:%d: expected an integer, got '%s'", path, lineno, value);
            return false;
        }

        if (ns > 16666666) {
            LOG_AND_NOTIFY_ERR("%s:%d: timeout must not exceed 16ms", path, lineno);
            return false;
        }

        conf->tweak.delayed_render_upper_ns = ns;
        LOG_WARN("tweak: delayed-render-upper=%lu", ns);
    }

    else if (strcmp(key, "max-shm-pool-size-mb") == 0) {
        unsigned long mb;
        if (!str_to_ulong(value, 10, &mb)) {
            LOG_AND_NOTIFY_ERR("%s:%d: expected an integer, got '%s'", path, lineno, value);
            return false;
        }

        conf->tweak.max_shm_pool_size = min(mb * 1024 * 1024, INT32_MAX);
        LOG_WARN("tweak: max-shm-pool-size=%lld bytes",
                 (long long)conf->tweak.max_shm_pool_size);
    }

    else {
        LOG_AND_NOTIFY_ERR("%s:%u: [tweak]: %s: invalid key", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_config_file(FILE *f, struct config *conf, const char *path, bool errors_are_fatal)
{
    enum section {
        SECTION_MAIN,
        SECTION_SCROLLBACK,
        SECTION_COLORS,
        SECTION_CURSOR,
        SECTION_MOUSE,
        SECTION_CSD,
        SECTION_KEY_BINDINGS,
        SECTION_SEARCH_BINDINGS,
        SECTION_MOUSE_BINDINGS,
        SECTION_TWEAK,
        SECTION_COUNT,
    } section = SECTION_MAIN;

    /* Function pointer, called for each key/value line */
    typedef bool (*parser_fun_t)(
        const char *key, const char *value, struct config *conf,
        const char *path, unsigned lineno);

    static const struct {
        parser_fun_t fun;
        const char *name;
    } section_info[] = {
        [SECTION_MAIN] =            {&parse_section_main, "main"},
        [SECTION_SCROLLBACK] =      {&parse_section_scrollback, "scrollback"},
        [SECTION_COLORS] =          {&parse_section_colors, "colors"},
        [SECTION_CURSOR] =          {&parse_section_cursor, "cursor"},
        [SECTION_MOUSE] =           {&parse_section_mouse, "mouse"},
        [SECTION_CSD] =             {&parse_section_csd, "csd"},
        [SECTION_KEY_BINDINGS] =    {&parse_section_key_bindings, "key-bindings"},
        [SECTION_SEARCH_BINDINGS] = {&parse_section_search_bindings, "search-bindings"},
        [SECTION_MOUSE_BINDINGS] =  {&parse_section_mouse_bindings, "mouse-bindings"},
        [SECTION_TWEAK] =           {&parse_section_tweak, "tweak"},
    };

    static_assert(ALEN(section_info) == SECTION_COUNT, "section info array size mismatch");

    unsigned lineno = 0;

    char *_line = NULL;
    size_t count = 0;

#define error_or_continue()                     \
    {                                           \
        if (errors_are_fatal)                   \
            goto err;                           \
        else                                    \
            continue;                           \
    }

    while (true) {
        errno = 0;
        lineno++;

        ssize_t ret = getline(&_line, &count, f);

        if (ret < 0) {
            if (errno != 0) {
                LOG_AND_NOTIFY_ERRNO("failed to read from configuration");
                if (errors_are_fatal)
                    goto err;
            }
            break;
        }

        /* Strip leading whitespace */
        char *line = _line;
        {
            while (isspace(*line))
                line++;
            if (line[0] != '\0') {
                char *end = line + strlen(line) - 1;
                while (isspace(*end))
                    end--;
                *(end + 1) = '\0';
            }
        }

        /* Empty line, or comment */
        if (line[0] == '\0' || line[0] == '#')
            continue;

        /* Split up into key/value pair + trailing comment */
        char *key_value = strtok(line, "#");
        char UNUSED *comment = strtok(NULL, "\n");

        /* Check for new section */
        if (key_value[0] == '[') {
            char *end = strchr(key_value, ']');
            if (end == NULL) {
                LOG_AND_NOTIFY_ERR("%s:%d: syntax error: %s", path, lineno, key_value);
                error_or_continue();
            }

            *end = '\0';

            section = SECTION_COUNT;
            for (enum section i = 0; i < SECTION_COUNT; i++) {
                if (strcmp(&key_value[1], section_info[i].name) == 0) {
                    section = i;
                }
            }

            if (section == SECTION_COUNT) {
                LOG_AND_NOTIFY_ERR("%s:%d: invalid section name: %s", path, lineno, &key_value[1]);
                error_or_continue();
            }

            /* Process next line */
            continue;
        }

        char *key = strtok(key_value, "=");
        if (key == NULL) {
            LOG_AND_NOTIFY_ERR("%s:%d: syntax error: no key specified", path, lineno);
            error_or_continue();
        }

        char *value = strtok(NULL, "\n");
        if (value == NULL) {
            /* Empty value, i.e. "key=" */
            value = key + strlen(key);
        }

        /* Strip trailing whitespace from key (leading stripped earlier) */
        {
            assert(!isspace(*key));

            char *end = key + strlen(key) - 1;
            while (isspace(*end))
                end--;
            *(end + 1) = '\0';
        }

        /* Strip leading+trailing whitespace from value */
        {
            while (isspace(*value))
                value++;

            if (value[0] != '\0') {
                char *end = value + strlen(value) - 1;
                while (isspace(*end))
                    end--;
                *(end + 1) = '\0';
            }
        }

        LOG_DBG("section=%s, key='%s', value='%s', comment='%s'",
                section_info[section].name, key, value, comment);

        parser_fun_t section_parser = section_info[section].fun;
        assert(section_parser != NULL);

        if (!section_parser(key, value, conf, path, lineno))
            error_or_continue();
    }

    free(_line);
    return true;

err:
    free(_line);
    return false;
}

static char *
get_server_socket_path(void)
{
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime == NULL)
        return xstrdup("/tmp/foot.sock");

    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (wayland_display == NULL) {
        return xasprintf("%s/foot.sock", xdg_runtime);
    }

    return xasprintf("%s/foot-%s.sock", xdg_runtime, wayland_display);
}

static void
add_default_key_bindings(struct config *conf)
{
#define add_binding(action, mods, sym)                                  \
    do {                                                                \
        tll_push_back(                                                  \
            conf->bindings.key,                                         \
            ((struct config_key_binding_normal){action, mods, sym}));   \
    } while (0)

    const struct config_key_modifiers shift = {.shift = true};
    const struct config_key_modifiers ctrl = {.ctrl = true};
    const struct config_key_modifiers ctrl_shift = {.ctrl = true, .shift = true};

    add_binding(BIND_ACTION_SCROLLBACK_UP, shift, XKB_KEY_Page_Up);
    add_binding(BIND_ACTION_SCROLLBACK_DOWN, shift, XKB_KEY_Page_Down);
    add_binding(BIND_ACTION_CLIPBOARD_COPY, ctrl_shift, XKB_KEY_C);
    add_binding(BIND_ACTION_CLIPBOARD_PASTE, ctrl_shift, XKB_KEY_V);
    add_binding(BIND_ACTION_SEARCH_START, ctrl_shift, XKB_KEY_R);
    add_binding(BIND_ACTION_FONT_SIZE_UP, ctrl, XKB_KEY_plus);
    add_binding(BIND_ACTION_FONT_SIZE_UP, ctrl, XKB_KEY_equal);
    add_binding(BIND_ACTION_FONT_SIZE_UP, ctrl, XKB_KEY_KP_Add);
    add_binding(BIND_ACTION_FONT_SIZE_DOWN, ctrl, XKB_KEY_minus);
    add_binding(BIND_ACTION_FONT_SIZE_DOWN, ctrl, XKB_KEY_KP_Subtract);
    add_binding(BIND_ACTION_FONT_SIZE_RESET, ctrl, XKB_KEY_0);
    add_binding(BIND_ACTION_FONT_SIZE_RESET, ctrl, XKB_KEY_KP_0);
    add_binding(BIND_ACTION_SPAWN_TERMINAL, ctrl_shift, XKB_KEY_N);

#undef add_binding
}

static void
add_default_search_bindings(struct config *conf)
{
#define add_binding(action, mods, sym)                                  \
    do {                                                                \
        tll_push_back(                                                  \
            conf->bindings.search,                                      \
            ((struct config_key_binding_search){action, mods, sym}));   \
} while (0)

    const struct config_key_modifiers none = {0};
    const struct config_key_modifiers alt = {.alt = true};
    const struct config_key_modifiers ctrl = {.ctrl = true};
    const struct config_key_modifiers ctrl_shift = {.ctrl = true, .shift = true};

    add_binding(BIND_ACTION_SEARCH_CANCEL, ctrl, XKB_KEY_g);
    add_binding(BIND_ACTION_SEARCH_CANCEL, none, XKB_KEY_Escape);
    add_binding(BIND_ACTION_SEARCH_COMMIT, none, XKB_KEY_Return);
    add_binding(BIND_ACTION_SEARCH_FIND_PREV, ctrl, XKB_KEY_r);
    add_binding(BIND_ACTION_SEARCH_FIND_NEXT, ctrl, XKB_KEY_s);
    add_binding(BIND_ACTION_SEARCH_EDIT_LEFT, none, XKB_KEY_Left);
    add_binding(BIND_ACTION_SEARCH_EDIT_LEFT, ctrl, XKB_KEY_b);
    add_binding(BIND_ACTION_SEARCH_EDIT_LEFT_WORD, ctrl, XKB_KEY_Left);
    add_binding(BIND_ACTION_SEARCH_EDIT_LEFT_WORD, alt, XKB_KEY_b);
    add_binding(BIND_ACTION_SEARCH_EDIT_RIGHT, none, XKB_KEY_Right);
    add_binding(BIND_ACTION_SEARCH_EDIT_RIGHT, ctrl, XKB_KEY_f);
    add_binding(BIND_ACTION_SEARCH_EDIT_RIGHT_WORD, ctrl, XKB_KEY_Right);
    add_binding(BIND_ACTION_SEARCH_EDIT_RIGHT_WORD, alt, XKB_KEY_f);
    add_binding(BIND_ACTION_SEARCH_EDIT_HOME, none, XKB_KEY_Home);
    add_binding(BIND_ACTION_SEARCH_EDIT_HOME, ctrl, XKB_KEY_a);
    add_binding(BIND_ACTION_SEARCH_EDIT_END, none, XKB_KEY_End);
    add_binding(BIND_ACTION_SEARCH_EDIT_END, ctrl, XKB_KEY_e);
    add_binding(BIND_ACTION_SEARCH_DELETE_PREV, none, XKB_KEY_BackSpace);
    add_binding(BIND_ACTION_SEARCH_DELETE_PREV_WORD, ctrl, XKB_KEY_BackSpace);
    add_binding(BIND_ACTION_SEARCH_DELETE_PREV_WORD, alt, XKB_KEY_BackSpace);
    add_binding(BIND_ACTION_SEARCH_DELETE_NEXT, none, XKB_KEY_Delete);
    add_binding(BIND_ACTION_SEARCH_DELETE_NEXT_WORD, ctrl, XKB_KEY_Delete);
    add_binding(BIND_ACTION_SEARCH_DELETE_NEXT_WORD, alt, XKB_KEY_d);
    add_binding(BIND_ACTION_SEARCH_EXTEND_WORD, ctrl, XKB_KEY_w);
    add_binding(BIND_ACTION_SEARCH_EXTEND_WORD_WS, ctrl_shift, XKB_KEY_W);

#undef add_binding
}

static void
add_default_mouse_bindings(struct config *conf)
{
#define add_binding(action, mods, btn, count)                           \
    do {                                                                \
        tll_push_back(                                                  \
            conf->bindings.mouse,                                       \
            ((struct config_mouse_binding){action, mods, btn, count})); \
} while (0)

    const struct config_key_modifiers none = {0};
    const struct config_key_modifiers ctrl = {.ctrl = true};

    add_binding(BIND_ACTION_PRIMARY_PASTE, none, BTN_MIDDLE, 1);
    add_binding(BIND_ACTION_SELECT_BEGIN, none, BTN_LEFT, 1);
    add_binding(BIND_ACTION_SELECT_BEGIN_BLOCK, ctrl, BTN_LEFT, 1);
    add_binding(BIND_ACTION_SELECT_EXTEND, none, BTN_RIGHT, 1);
    add_binding(BIND_ACTION_SELECT_WORD, none, BTN_LEFT, 2);
    add_binding(BIND_ACTION_SELECT_WORD_WS, ctrl, BTN_LEFT, 2);
    add_binding(BIND_ACTION_SELECT_ROW, none, BTN_LEFT, 3);

#undef add_binding
}

bool
config_load(struct config *conf, const char *conf_path, bool errors_are_fatal)
{
    bool ret = false;

    *conf = (struct config) {
        .term = xstrdup("foot"),
        .shell = get_shell(),
        .title = xstrdup("foot"),
        .app_id = xstrdup("foot"),
        .width = 700,
        .height = 500,
        .pad_x = 2,
        .pad_y = 2,
        .startup_mode = STARTUP_WINDOWED,
        .fonts = tll_init(),
        .scrollback = {
            .lines = 1000,
            .indicator = {
                .position = SCROLLBACK_INDICATOR_POSITION_RELATIVE,
                .format = SCROLLBACK_INDICATOR_FORMAT_TEXT,
                .text = wcsdup(L""),
            },
            .multiplier = 1.,
        },
        .colors = {
            .fg = default_foreground,
            .bg = default_background,
            .regular = {
                default_regular[0],
                default_regular[1],
                default_regular[2],
                default_regular[3],
                default_regular[4],
                default_regular[5],
                default_regular[6],
                default_regular[7],
            },
            .bright = {
                default_bright[0],
                default_bright[1],
                default_bright[2],
                default_bright[3],
                default_bright[4],
                default_bright[5],
                default_bright[6],
                default_bright[7],
            },
            .alpha = 0xffff,
            .selection_fg = 0x80000000,  /* Use default bg */
            .selection_bg = 0x80000000,  /* Use default fg */
            .selection_uses_custom_colors = false,
        },

        .cursor = {
            .style = CURSOR_BLOCK,
            .blink = false,
            .color = {
                .text = 0,
                .cursor = 0,
            },
        },
        .mouse = {
            .hide_when_typing = false,
        },
        .csd = {
            .preferred = CONF_CSD_PREFER_SERVER,
            .title_height = 26,
            .border_width = 5,
            .button_width = 26,
        },

        .render_worker_count = sysconf(_SC_NPROCESSORS_ONLN),
        .server_socket_path = get_server_socket_path(),
        .presentation_timings = false,
        .hold_at_exit = false,

        .tweak = {
            .delayed_render_lower_ns = 500000,         /* 0.5ms */
            .delayed_render_upper_ns = 16666666 / 2,   /* half a frame period (60Hz) */
            .max_shm_pool_size = 512 * 1024 * 1024,
            .render_timer_osd = false,
            .render_timer_log = false,
        },

        .notifications = tll_init(),
    };

    add_default_key_bindings(conf);
    add_default_search_bindings(conf);
    add_default_mouse_bindings(conf);

    struct config_file conf_file = {.path = NULL, .fd = -1};
    if (conf_path != NULL) {
        int fd = open(conf_path, O_RDONLY);
        if (fd < 0) {
            LOG_AND_NOTIFY_ERRNO("%s: failed to open", conf_path);
            ret = !errors_are_fatal;
            goto out;
        }

        conf_file.path = xstrdup(conf_path);
        conf_file.fd = fd;
    } else {
        conf_file = open_config(conf);
        if (conf_file.fd < 0) {
            LOG_AND_NOTIFY_ERR("no configuration found, using defaults");
            ret = !errors_are_fatal;
            goto out;
        }
    }

    assert(conf_file.path != NULL);
    assert(conf_file.fd >= 0);
    LOG_INFO("loading configuration from %s", conf_file.path);

    FILE *f = fdopen(conf_file.fd, "r");
    if (f == NULL) {
        LOG_AND_NOTIFY_ERRNO("%s: failed to open", conf_file.path);
        ret = !errors_are_fatal;
        goto out;
    }

    ret = parse_config_file(f, conf, conf_path, errors_are_fatal);
    fclose(f);

    conf->colors.selection_uses_custom_colors =
        conf->colors.selection_fg >> 24 == 0 &&
        conf->colors.selection_bg >> 24 == 0;

out:
    if (ret && tll_length(conf->fonts) == 0)
        tll_push_back(conf->fonts, config_font_parse("monospace"));

    free(conf_file.path);
    if (conf_file.fd < 0)
        close(conf_file.fd);

    return ret;
}

void
config_free(struct config conf)
{
    free(conf.term);
    free(conf.shell);
    free(conf.title);
    free(conf.app_id);
    free(conf.scrollback.indicator.text);
    tll_foreach(conf.fonts, it)
        config_font_destroy(&it->item);
    tll_free(conf.fonts);
    free(conf.server_socket_path);

    tll_foreach(conf.bindings.key, it) {
        if (it->item.pipe.master_copy) {
            free(it->item.pipe.cmd);
            free(it->item.pipe.argv);
        }
    }

    tll_free(conf.bindings.key);
    tll_free(conf.bindings.mouse);
    tll_free(conf.bindings.search);

    tll_foreach(conf.notifications, it)
        free(it->item.text);
    tll_free(conf.notifications);
}

struct config_font
config_font_parse(const char *pattern)
{
    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);

    double pt_size = -1.0;
    FcPatternGetDouble(pat, FC_SIZE, 0, &pt_size);
    FcPatternRemove(pat, FC_SIZE, 0);

    int px_size = -1;
    FcPatternGetInteger(pat, FC_PIXEL_SIZE, 0, &px_size);
    FcPatternRemove(pat, FC_PIXEL_SIZE, 0);

    if (pt_size == -1. && px_size == -1)
        pt_size = 8.0;

    char *stripped_pattern = (char *)FcNameUnparse(pat);
    FcPatternDestroy(pat);

    return (struct config_font){
        .pattern = stripped_pattern,
        .pt_size = pt_size,
        .px_size = px_size};
}

void
config_font_destroy(struct config_font *font)
{
    if (font == NULL)
        return;
    free(font->pattern);
}
