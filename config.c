#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
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
#include "char32.h"
#include "debug.h"
#include "input.h"
#include "key-binding.h"
#include "macros.h"
#include "tokenize.h"
#include "util.h"
#include "xmalloc.h"
#include "xsnprintf.h"

static const uint32_t default_foreground = 0xdcdccc;
static const uint32_t default_background = 0x111111;

static const size_t min_csd_border_width = 5;

#define cube6(r, g) \
    r|g|0x00, r|g|0x5f, r|g|0x87, r|g|0xaf, r|g|0xd7, r|g|0xff

#define cube36(r) \
    cube6(r, 0x0000), \
    cube6(r, 0x5f00), \
    cube6(r, 0x8700), \
    cube6(r, 0xaf00), \
    cube6(r, 0xd700), \
    cube6(r, 0xff00)

static const uint32_t default_color_table[256] = {
    // Regular
    0x222222,
    0xcc9393,
    0x7f9f7f,
    0xd0bf8f,
    0x6ca0a3,
    0xdc8cc3,
    0x93e0e3,
    0xdcdccc,

    // Bright
    0x666666,
    0xdca3a3,
    0xbfebbf,
    0xf0dfaf,
    0x8cd0d3,
    0xfcace3,
    0xb3ffff,
    0xffffff,

    // 6x6x6 RGB cube
    // (color channels = i ? i*40+55 : 0, where i = 0..5)
    cube36(0x000000),
    cube36(0x5f0000),
    cube36(0x870000),
    cube36(0xaf0000),
    cube36(0xd70000),
    cube36(0xff0000),

    // 24 shades of gray
    // (color channels = i*10+8, where i = 0..23)
    0x080808, 0x121212, 0x1c1c1c, 0x262626,
    0x303030, 0x3a3a3a, 0x444444, 0x4e4e4e,
    0x585858, 0x626262, 0x6c6c6c, 0x767676,
    0x808080, 0x8a8a8a, 0x949494, 0x9e9e9e,
    0xa8a8a8, 0xb2b2b2, 0xbcbcbc, 0xc6c6c6,
    0xd0d0d0, 0xdadada, 0xe4e4e4, 0xeeeeee
};

static const char *const binding_action_map[] = {
    [BIND_ACTION_NONE] = NULL,
    [BIND_ACTION_NOOP] = "noop",
    [BIND_ACTION_SCROLLBACK_UP_PAGE] = "scrollback-up-page",
    [BIND_ACTION_SCROLLBACK_UP_HALF_PAGE] = "scrollback-up-half-page",
    [BIND_ACTION_SCROLLBACK_UP_LINE] = "scrollback-up-line",
    [BIND_ACTION_SCROLLBACK_DOWN_PAGE] = "scrollback-down-page",
    [BIND_ACTION_SCROLLBACK_DOWN_HALF_PAGE] = "scrollback-down-half-page",
    [BIND_ACTION_SCROLLBACK_DOWN_LINE] = "scrollback-down-line",
    [BIND_ACTION_SCROLLBACK_HOME] = "scrollback-home",
    [BIND_ACTION_SCROLLBACK_END] = "scrollback-end",
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
    [BIND_ACTION_SHOW_URLS_COPY] = "show-urls-copy",
    [BIND_ACTION_SHOW_URLS_LAUNCH] = "show-urls-launch",
    [BIND_ACTION_SHOW_URLS_PERSISTENT] = "show-urls-persistent",
    [BIND_ACTION_TEXT_BINDING] = "text-binding",
    [BIND_ACTION_PROMPT_PREV] = "prompt-prev",
    [BIND_ACTION_PROMPT_NEXT] = "prompt-next",

    /* Mouse-specific actions */
    [BIND_ACTION_SELECT_BEGIN] = "select-begin",
    [BIND_ACTION_SELECT_BEGIN_BLOCK] = "select-begin-block",
    [BIND_ACTION_SELECT_EXTEND] = "select-extend",
    [BIND_ACTION_SELECT_EXTEND_CHAR_WISE] = "select-extend-character-wise",
    [BIND_ACTION_SELECT_WORD] = "select-word",
    [BIND_ACTION_SELECT_WORD_WS] = "select-word-whitespace",
    [BIND_ACTION_SELECT_ROW] = "select-row",
};

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
    [BIND_ACTION_SEARCH_CLIPBOARD_PASTE] = "clipboard-paste",
    [BIND_ACTION_SEARCH_PRIMARY_PASTE] = "primary-paste",
};

static const char *const url_binding_action_map[] = {
    [BIND_ACTION_URL_NONE] = NULL,
    [BIND_ACTION_URL_CANCEL] = "cancel",
    [BIND_ACTION_URL_TOGGLE_URL_ON_JUMP_LABEL] = "toggle-url-visible",
};

static_assert(ALEN(binding_action_map) == BIND_ACTION_COUNT,
              "binding action map size mismatch");
static_assert(ALEN(search_binding_action_map) == BIND_ACTION_SEARCH_COUNT,
              "search binding action map size mismatch");
static_assert(ALEN(url_binding_action_map) == BIND_ACTION_URL_COUNT,
              "URL binding action map size mismatch");

struct context {
    struct config *conf;
    const char *section;
    const char *key;
    const char *value;

    const char *path;
    unsigned lineno;

    bool errors_are_fatal;
};

static const enum user_notification_kind log_class_to_notify_kind[LOG_CLASS_COUNT] = {
    [LOG_CLASS_WARNING] = USER_NOTIFICATION_WARNING,
    [LOG_CLASS_ERROR] = USER_NOTIFICATION_ERROR,
};

static void NOINLINE VPRINTF(5)
log_and_notify_va(struct config *conf, enum log_class log_class,
                  const char *file, int lineno, const char *fmt, va_list va)
{
    xassert(log_class < ALEN(log_class_to_notify_kind));
    enum user_notification_kind kind = log_class_to_notify_kind[log_class];

    if (kind == 0) {
        BUG("unsupported log class: %d", (int)log_class);
        return;
    }

    char *formatted_msg = xvasprintf(fmt, va);
    log_msg(log_class, LOG_MODULE, file, lineno, "%s", formatted_msg);
    user_notification_add(&conf->notifications, kind, formatted_msg);
}

static void NOINLINE PRINTF(5)
log_and_notify(struct config *conf, enum log_class log_class,
               const char *file, int lineno, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    log_and_notify_va(conf, log_class, file, lineno, fmt, va);
    va_end(va);
}

static void NOINLINE PRINTF(5)
log_contextual(struct context *ctx, enum log_class log_class,
               const char *file, int lineno, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    char *formatted_msg = xvasprintf(fmt, va);
    va_end(va);

    bool print_dot = ctx->key != NULL;
    bool print_colon = ctx->value != NULL;

    if (!print_dot)
        ctx->key = "";

    if (!print_colon)
        ctx->value = "";

    log_and_notify(
        ctx->conf, log_class, file, lineno, "%s:%d: [%s]%s%s%s%s: %s",
        ctx->path, ctx->lineno, ctx->section, print_dot ? "." : "",
        ctx->key, print_colon ? ": " : "", ctx->value, formatted_msg);
    free(formatted_msg);
}


static void NOINLINE VPRINTF(4)
log_and_notify_errno_va(struct config *conf, const char *file, int lineno,
                     const char *fmt, va_list va)
{
    int errno_copy = errno;
    char *formatted_msg = xvasprintf(fmt, va);
    log_and_notify(
        conf, LOG_CLASS_ERROR, file, lineno,
        "%s: %s", formatted_msg, strerror(errno_copy));
    free(formatted_msg);
}

static void NOINLINE PRINTF(4)
log_and_notify_errno(struct config *conf, const char *file, int lineno,
                     const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    log_and_notify_errno_va(conf, file, lineno, fmt, va);
    va_end(va);
}

static void NOINLINE PRINTF(4)
log_contextual_errno(struct context *ctx, const char *file, int lineno,
                     const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    char *formatted_msg = xvasprintf(fmt, va);
    va_end(va);

    log_and_notify_errno(
        ctx->conf, file, lineno, "%s:%d: [%s].%s: %s: %s",
        ctx->path, ctx->lineno, ctx->section, ctx->key, ctx->value,
        formatted_msg);

    free(formatted_msg);
}

#define LOG_CONTEXTUAL_ERR(...) \
    log_contextual(ctx, LOG_CLASS_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_CONTEXTUAL_WARN(...) \
    log_contextual(ctx, LOG_CLASS_WARNING, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_CONTEXTUAL_ERRNO(...) \
    log_contextual_errno(ctx, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_AND_NOTIFY_ERR(...) \
    log_and_notify(conf, LOG_CLASS_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_AND_NOTIFY_WARN(...) \
    log_and_notify(conf, LOG_CLASS_WARNING, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_AND_NOTIFY_ERRNO(...) \
    log_and_notify_errno(conf, __FILE__, __LINE__, __VA_ARGS__)

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

static struct config_file
open_config(void)
{
    char *path = NULL;
    struct config_file ret = {.path = NULL, .fd = -1};

    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char *xdg_config_dirs = getenv("XDG_CONFIG_DIRS");
    const char *home_dir = getenv("HOME");
    char *xdg_config_dirs_copy = NULL;

    /* First, check XDG_CONFIG_HOME (or .config, if unset) */
    if (xdg_config_home != NULL && xdg_config_home[0] != '\0')
        path = xasprintf("%s/foot/foot.ini", xdg_config_home);
    else if (home_dir != NULL)
        path = xasprintf("%s/.config/foot/foot.ini", home_dir);

    if (path != NULL) {
        LOG_DBG("checking for %s", path);
        int fd = open(path, O_RDONLY | O_CLOEXEC);

        if (fd >= 0) {
            ret = (struct config_file) {.path = path, .fd = fd};
            path = NULL;
            goto done;
        }
    }

    xdg_config_dirs_copy = xdg_config_dirs != NULL && xdg_config_dirs[0] != '\0'
        ? strdup(xdg_config_dirs)
        : strdup("/etc/xdg");

    if (xdg_config_dirs_copy == NULL || xdg_config_dirs_copy[0] == '\0')
        goto done;

    for (const char *conf_dir = strtok(xdg_config_dirs_copy, ":");
         conf_dir != NULL;
         conf_dir = strtok(NULL, ":"))
    {
        free(path);
        path = xasprintf("%s/foot/foot.ini", conf_dir);

        LOG_DBG("checking for %s", path);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ret = (struct config_file){.path = path, .fd = fd};
            path = NULL;
            goto done;
        }
    }

done:
    free(xdg_config_dirs_copy);
    free(path);
    return ret;
}

static int
c32cmp_single(const void *_a, const void *_b)
{
    const char32_t *a = _a;
    const char32_t *b = _b;
    return *a - *b;
}

static bool
str_has_prefix(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static bool NOINLINE
value_to_bool(struct context *ctx, bool *res)
{
    static const char *const yes[] = {"on", "true", "yes", "1"};
    static const char *const  no[] = {"off", "false", "no", "0"};

    for (size_t i = 0; i < ALEN(yes); i++) {
        if (strcasecmp(ctx->value, yes[i]) == 0) {
            *res = true;
            return true;
        }
    }

    for (size_t i = 0; i < ALEN(no); i++) {
        if (strcasecmp(ctx->value, no[i]) == 0) {
            *res = false;
            return true;
        }
    }

    LOG_CONTEXTUAL_ERR("invalid boolean value");
    return false;
}


static bool NOINLINE
str_to_ulong(const char *s, int base, unsigned long *res)
{
    if (s == NULL)
        return false;

    errno = 0;
    char *end = NULL;

    *res = strtoul(s, &end, base);
    return errno == 0 && *end == '\0';
}

static bool NOINLINE
str_to_uint32(const char *s, int base, uint32_t *res)
{
    unsigned long v;
    bool ret = str_to_ulong(s, base, &v);
    if (v > UINT32_MAX)
        return false;
    *res = v;
    return ret;
}

static bool NOINLINE
str_to_uint16(const char *s, int base, uint16_t *res)
{
    unsigned long v;
    bool ret = str_to_ulong(s, base, &v);
    if (v > UINT16_MAX)
        return false;
    *res = v;
    return ret;
}

static bool NOINLINE
value_to_uint16(struct context *ctx, int base, uint16_t *res)
{
    if (!str_to_uint16(ctx->value, base, res)) {
        LOG_CONTEXTUAL_ERR(
            "invalid integer value, or outside range 0-%u", UINT16_MAX);
        return false;
    }
    return true;
}

static bool NOINLINE
value_to_uint32(struct context *ctx, int base, uint32_t *res)
{
    if (!str_to_uint32(ctx->value, base, res)){
        LOG_CONTEXTUAL_ERR(
            "invalid integer value, or outside range 0-%u", UINT32_MAX);
        return false;
    }
    return true;
}

static bool NOINLINE
value_to_dimensions(struct context *ctx, uint32_t *x, uint32_t *y)
{
    if (sscanf(ctx->value, "%ux%u", x, y) != 2) {
        LOG_CONTEXTUAL_ERR("invalid dimensions (must be in the form AxB)");
        return false;
    }

    return true;
}

static bool NOINLINE
value_to_double(struct context *ctx, float *res)
{
    const char *s = ctx->value;

    if (s == NULL)
        return false;

    errno = 0;
    char *end = NULL;

    *res = strtof(s, &end);
    if (!(errno == 0 && *end == '\0')) {
        LOG_CONTEXTUAL_ERR("invalid decimal value");
        return false;
    }

    return true;
}

static bool NOINLINE
value_to_str(struct context *ctx, char **res)
{
    free(*res);
    *res = xstrdup(ctx->value);
    return true;
}

static bool NOINLINE
value_to_wchars(struct context *ctx, char32_t **res)
{
    char32_t *s = ambstoc32(ctx->value);
    if (s == NULL) {
        LOG_CONTEXTUAL_ERR("not a valid string value");
        return false;
    }

    free(*res);
    *res = s;
    return true;
}

static bool NOINLINE
value_to_enum(struct context *ctx, const char **value_map, int *res)
{
    size_t str_len = 0;
    size_t count = 0;

    for (; value_map[count] != NULL; count++) {
        if (strcasecmp(value_map[count], ctx->value) == 0) {
            *res = count;
            return true;
        }
        str_len += strlen(value_map[count]);
    }

    const size_t size = str_len + count * 4 + 1;
    char valid_values[512];
    size_t idx = 0;
    xassert(size < sizeof(valid_values));

    for (size_t i = 0; i < count; i++)
        idx += xsnprintf(&valid_values[idx], size - idx, "'%s', ", value_map[i]);

    if (count > 0)
        valid_values[idx - 2] = '\0';

    LOG_CONTEXTUAL_ERR("not one of %s", valid_values);
    *res = -1;
    return false;
}

static bool NOINLINE
value_to_color(struct context *ctx, uint32_t *color, bool allow_alpha)
{
    if (!str_to_uint32(ctx->value, 16, color)) {
        LOG_CONTEXTUAL_ERR("not a valid color value");
        return false;
    }

    if (!allow_alpha && (*color & 0xff000000) != 0) {
        LOG_CONTEXTUAL_ERR("color value must not have an alpha component");
        return false;
    }

    return true;
}

static bool NOINLINE
value_to_two_colors(struct context *ctx,
                    uint32_t *first, uint32_t *second, bool allow_alpha)
{
    bool ret = false;
    const char *original_value = ctx->value;

    /* TODO: do this without strdup() */
    char *value_copy = xstrdup(ctx->value);
    const char *first_as_str = strtok(value_copy, " ");
    const char *second_as_str = strtok(NULL, " ");

    if (first_as_str == NULL || second_as_str == NULL) {
        LOG_CONTEXTUAL_ERR("invalid double color value");
        goto out;
    }

    ctx->value = first_as_str;
    if (!value_to_color(ctx, first, allow_alpha))
        goto out;

    ctx->value = second_as_str;
    if (!value_to_color(ctx, second, allow_alpha))
        goto out;

    ret = true;

out:
    free(value_copy);
    ctx->value = original_value;
    return ret;
}

static bool NOINLINE
value_to_pt_or_px(struct context *ctx, struct pt_or_px *res)
{
    const char *s = ctx->value;

    size_t len = s != NULL ? strlen(s) : 0;
    if (len >= 2 && s[len - 2] == 'p' && s[len - 1] == 'x') {
        errno = 0;
        char *end = NULL;

        long value = strtol(s, &end, 10);
        if (!(errno == 0 && end == s + len - 2)) {
            LOG_CONTEXTUAL_ERR("invalid px value (must be in the form 12px)");
            return false;
        }
        res->pt = 0;
        res->px = value;
    } else {
        float value;
        if (!value_to_double(ctx, &value))
            return false;
        res->pt = value;
        res->px = 0;
    }

    return true;
}

static struct config_font_list NOINLINE
value_to_fonts(struct context *ctx)
{
    size_t count = 0;
    size_t size = 0;
    struct config_font *fonts = NULL;

    char *copy = xstrdup(ctx->value);
    for (const char *font = strtok(copy, ",");
         font != NULL;
         font = strtok(NULL, ","))
    {
        /* Trim spaces, strictly speaking not necessary, but looks nice :) */
        while (isspace(font[0]))
            font++;

        if (font[0] == '\0')
            continue;

        struct config_font font_data;
        if (!config_font_parse(font, &font_data)) {
            ctx->value = font;
            LOG_CONTEXTUAL_ERR("invalid font specification");
            goto err;
        }

        if (count + 1 > size) {
            size += 4;
            fonts = xrealloc(fonts, size * sizeof(fonts[0]));
        }

        xassert(count + 1 <= size);
        fonts[count++] = font_data;
    }

    free(copy);
    return (struct config_font_list){.arr = fonts, .count = count};

err:
    free(copy);
    free(fonts);
    return (struct config_font_list){.arr = NULL, .count = 0};
}

static void NOINLINE
free_argv(struct argv *argv)
{
    if (argv->args == NULL)
        return;
    for (char **a = argv->args; *a != NULL; a++)
        free(*a);
    free(argv->args);
    argv->args = NULL;
}

static void NOINLINE
clone_argv(struct argv *dst, const struct argv *src)
{
    if (src->args == NULL) {
        dst->args = NULL;
        return;
    }

    size_t count = 0;
    for (char **args = src->args; *args != NULL; args++)
        count++;

    dst->args = xmalloc((count + 1) * sizeof(dst->args[0]));
    for (char **args_src = src->args, **args_dst = dst->args;
         *args_src != NULL; args_src++,
             args_dst++)
    {
        *args_dst = xstrdup(*args_src);
    }
    dst->args[count] = NULL;
}

static void
spawn_template_free(struct config_spawn_template *template)
{
    free_argv(&template->argv);
}

static void
spawn_template_clone(struct config_spawn_template *dst,
                     const struct config_spawn_template *src)
{
    clone_argv(&dst->argv, &src->argv);
}

static bool NOINLINE
value_to_spawn_template(struct context *ctx,
                        struct config_spawn_template *template)
{
    spawn_template_free(template);

    char **argv = NULL;

    if (!tokenize_cmdline(ctx->value, &argv)) {
        LOG_CONTEXTUAL_ERR("syntax error in command line");
        return false;
    }

    template->argv.args = argv;
    return true;
}

static bool parse_config_file(
    FILE *f, struct config *conf, const char *path, bool errors_are_fatal);

static bool
parse_section_main(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;
    const char *value = ctx->value;
    bool errors_are_fatal = ctx->errors_are_fatal;

    if (strcmp(key, "include") == 0) {
        char *_include_path = NULL;
        const char *include_path = NULL;

        if (value[0] == '~' && value[1] == '/') {
            const char *home_dir = getenv("HOME");

            if (home_dir == NULL) {
                LOG_CONTEXTUAL_ERRNO("failed to expand '~'");
                return false;
            }

            _include_path = xasprintf("%s/%s", home_dir, value + 2);
            include_path = _include_path;
        } else
            include_path = value;

        if (include_path[0] != '/') {
            LOG_CONTEXTUAL_ERR("not an absolute path");
            free(_include_path);
            return false;
        }

        FILE *include = fopen(include_path, "r");

        if (include == NULL) {
            LOG_CONTEXTUAL_ERRNO("failed to open");
            free(_include_path);
            return false;
        }

        bool ret = parse_config_file(
            include, conf, include_path, errors_are_fatal);
        fclose(include);

        LOG_INFO("imported sub-configuration from %s", include_path);
        free(_include_path);
        return ret;
    }

    else if (strcmp(key, "term") == 0)
        return value_to_str(ctx, &conf->term);

    else if (strcmp(key, "shell") == 0)
        return value_to_str(ctx, &conf->shell);

    else if (strcmp(key, "login-shell") == 0)
        return value_to_bool(ctx, &conf->login_shell);

    else if (strcmp(key, "title") == 0)
        return value_to_str(ctx, &conf->title);

    else if (strcmp(key, "locked-title") == 0)
        return value_to_bool(ctx, &conf->locked_title);

    else if (strcmp(key, "app-id") == 0)
        return value_to_str(ctx, &conf->app_id);

    else if (strcmp(key, "initial-window-size-pixels") == 0) {
        if (!value_to_dimensions(ctx, &conf->size.width, &conf->size.height))
            return false;

        conf->size.type = CONF_SIZE_PX;
        return true;
    }

    else if (strcmp(key, "initial-window-size-chars") == 0) {
        if (!value_to_dimensions(ctx, &conf->size.width, &conf->size.height))
            return false;

        conf->size.type = CONF_SIZE_CELLS;
        return true;
    }

    else if (strcmp(key, "pad") == 0) {
        unsigned x, y;
        char mode[16] = {0};

        int ret = sscanf(value, "%ux%u %15s", &x, &y, mode);
        bool center = strcasecmp(mode, "center") == 0;
        bool invalid_mode = !center && mode[0] != '\0';

        if ((ret != 2 && ret != 3) || invalid_mode) {
            LOG_CONTEXTUAL_ERR(
                "invalid padding (must be in the form PAD_XxPAD_Y [center])");
            return false;
        }

        conf->pad_x = x;
        conf->pad_y = y;
        conf->center = center;
        return true;
    }

    else if (strcmp(key, "resize-delay-ms") == 0)
        return value_to_uint16(ctx, 10, &conf->resize_delay_ms);

    else if (strcmp(key, "bold-text-in-bright") == 0) {
        if (strcmp(value, "palette-based") == 0) {
            conf->bold_in_bright.enabled = true;
            conf->bold_in_bright.palette_based = true;
        } else {
            if (!value_to_bool(ctx, &conf->bold_in_bright.enabled))
                return false;
            conf->bold_in_bright.palette_based = false;
        }
        return true;
    }

    else if (strcmp(key, "initial-window-mode") == 0) {
        _Static_assert(sizeof(conf->startup_mode) == sizeof(int),
            "enum is not 32-bit");

        return value_to_enum(
                ctx,
                (const char *[]){"windowed", "maximized", "fullscreen", NULL},
                (int *)&conf->startup_mode);
    }

    else if (strcmp(key, "font") == 0 ||
             strcmp(key, "font-bold") == 0 ||
             strcmp(key, "font-italic") == 0 ||
             strcmp(key, "font-bold-italic") == 0)

    {
        size_t idx =
            strcmp(key, "font") == 0 ? 0 :
            strcmp(key, "font-bold") == 0 ? 1 :
            strcmp(key, "font-italic") == 0 ? 2 : 3;

        struct config_font_list new_list = value_to_fonts(ctx);
        if (new_list.arr == NULL)
            return false;

        config_font_list_destroy(&conf->fonts[idx]);
        conf->fonts[idx] = new_list;
        return true;
    }

    else if (strcmp(key, "line-height") == 0)
        return value_to_pt_or_px(ctx, &conf->line_height);

    else if (strcmp(key, "letter-spacing") == 0)
        return value_to_pt_or_px(ctx, &conf->letter_spacing);

    else if (strcmp(key, "horizontal-letter-offset") == 0)
        return value_to_pt_or_px(ctx, &conf->horizontal_letter_offset);

    else if (strcmp(key, "vertical-letter-offset") == 0)
        return value_to_pt_or_px(ctx, &conf->vertical_letter_offset);

    else if (strcmp(key, "underline-offset") == 0) {
        if (!value_to_pt_or_px(ctx, &conf->underline_offset))
            return false;
        conf->use_custom_underline_offset = true;
        return true;
    }

    else if (strcmp(key, "dpi-aware") == 0) {
        if (strcmp(value, "auto") == 0)
            conf->dpi_aware = DPI_AWARE_AUTO;
        else {
            bool value;
            if (!value_to_bool(ctx, &value))
                return false;
            conf->dpi_aware = value ? DPI_AWARE_YES : DPI_AWARE_NO;
        }
        return true;
    }

    else if (strcmp(key, "workers") == 0)
        return value_to_uint16(ctx, 10, &conf->render_worker_count);

    else if (strcmp(key, "word-delimiters") == 0)
        return value_to_wchars(ctx, &conf->word_delimiters);

    else if (strcmp(key, "notify") == 0)
        return value_to_spawn_template(ctx, &conf->notify);

    else if (strcmp(key, "notify-focus-inhibit") == 0)
        return value_to_bool(ctx, &conf->notify_focus_inhibit);

    else if (strcmp(key, "selection-target") == 0) {
        _Static_assert(sizeof(conf->selection_target) == sizeof(int),
                       "enum is not 32-bit");

        return value_to_enum(
            ctx,
            (const char *[]){"none", "primary", "clipboard", "both", NULL},
            (int *)&conf->selection_target);
    }

    else if (strcmp(key, "box-drawings-uses-font-glyphs") == 0)
        return value_to_bool(ctx, &conf->box_drawings_uses_font_glyphs);

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }
}

static bool
parse_section_bell(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "urgent") == 0)
        return value_to_bool(ctx, &conf->bell.urgent);
    else if (strcmp(key, "notify") == 0)
        return value_to_bool(ctx, &conf->bell.notify);
    else if (strcmp(key, "command") == 0)
        return value_to_spawn_template(ctx, &conf->bell.command);
    else if (strcmp(key, "command-focused") == 0)
        return value_to_bool(ctx, &conf->bell.command_focused);
    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }
}

static bool
parse_section_scrollback(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;
    const char *value = ctx->value;

    if (strcmp(key, "lines") == 0)
        return value_to_uint32(ctx, 10, &conf->scrollback.lines);

    else if (strcmp(key, "indicator-position") == 0) {
        _Static_assert(
            sizeof(conf->scrollback.indicator.position) == sizeof(int),
            "enum is not 32-bit");

        return value_to_enum(
            ctx,
            (const char *[]){"none", "fixed", "relative", NULL},
            (int *)&conf->scrollback.indicator.position);
    }

    else if (strcmp(key, "indicator-format") == 0) {
        if (strcmp(value, "percentage") == 0) {
            conf->scrollback.indicator.format
                = SCROLLBACK_INDICATOR_FORMAT_PERCENTAGE;
            return true;
        } else if (strcmp(value, "line") == 0) {
            conf->scrollback.indicator.format
                = SCROLLBACK_INDICATOR_FORMAT_LINENO;
            return true;
        } else
            return value_to_wchars(ctx, &conf->scrollback.indicator.text);
    }

    else if (strcmp(key, "multiplier") == 0)
        return value_to_double(ctx, &conf->scrollback.multiplier);

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }
}

static bool
parse_section_url(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;
    const char *value = ctx->value;

    if (strcmp(key, "launch") == 0)
        return value_to_spawn_template(ctx, &conf->url.launch);

    else if (strcmp(key, "label-letters") == 0)
        return value_to_wchars(ctx, &conf->url.label_letters);

    else if (strcmp(key, "osc8-underline") == 0) {
        _Static_assert(sizeof(conf->url.osc8_underline) == sizeof(int),
                       "enum is not 32-bit");

        return value_to_enum(
            ctx,
            (const char *[]){"url-mode", "always", NULL},
            (int *)&conf->url.osc8_underline);
    }

    else if (strcmp(key, "protocols") == 0) {
        for (size_t i = 0; i < conf->url.prot_count; i++)
            free(conf->url.protocols[i]);
        free(conf->url.protocols);

        conf->url.max_prot_len = 0;
        conf->url.prot_count = 0;
        conf->url.protocols = NULL;

        char *copy = xstrdup(value);

        for (char *prot = strtok(copy, ",");
             prot != NULL;
             prot = strtok(NULL, ","))
        {

            /* Strip leading whitespace */
            while (isspace(prot[0]))
                prot++;

            /* Strip trailing whitespace */
            size_t len = strlen(prot);
            while (isspace(prot[len - 1]))
                len--;
            prot[len] = '\0';

            size_t chars = mbsntoc32(NULL, prot, len, 0);
            if (chars == (size_t)-1) {
                ctx->value = prot;
                LOG_CONTEXTUAL_ERRNO("invalid protocol");
                return false;
            }

            conf->url.prot_count++;
            conf->url.protocols = xrealloc(
                conf->url.protocols,
                conf->url.prot_count * sizeof(conf->url.protocols[0]));

            size_t idx = conf->url.prot_count - 1;
            conf->url.protocols[idx] = xmalloc((chars + 1 + 3) * sizeof(char32_t));
            mbsntoc32(conf->url.protocols[idx], prot, len, chars + 1);
            c32cpy(&conf->url.protocols[idx][chars], U"://");

            chars += 3;  /* Include the "://" */
            if (chars > conf->url.max_prot_len)
                conf->url.max_prot_len = chars;
        }

        free(copy);
        return true;
    }

    else if (strcmp(key, "uri-characters") == 0) {
        if (!value_to_wchars(ctx, &conf->url.uri_characters))
            return false;

        qsort(
            conf->url.uri_characters,
            c32len(conf->url.uri_characters),
            sizeof(conf->url.uri_characters[0]),
            &c32cmp_single);
        return true;
    }

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }
}

static bool
parse_section_colors(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    size_t key_len = strlen(key);
    uint8_t last_digit = (unsigned char)key[key_len - 1] - '0';
    uint32_t *color = NULL;

    if (isdigit(key[0])) {
        unsigned long index;
        if (!str_to_ulong(key, 0, &index) ||
            index >= ALEN(conf->colors.table))
        {
            LOG_CONTEXTUAL_ERR(
                "invalid color palette index: %s (not in range 0-%zu)",
                key, ALEN(conf->colors.table));
            return false;
        }
        color = &conf->colors.table[index];
    }

    else if (key_len == 8 && str_has_prefix(key, "regular") && last_digit < 8)
        color = &conf->colors.table[last_digit];

    else if (key_len == 7 && str_has_prefix(key, "bright") && last_digit < 8)
        color = &conf->colors.table[8 + last_digit];

    else if (key_len == 4 && str_has_prefix(key, "dim") && last_digit < 8) {
        if (!value_to_color(ctx, &conf->colors.dim[last_digit], false))
            return false;

        conf->colors.use_custom.dim |= 1 << last_digit;
        return true;
    }

    else if (strcmp(key, "foreground") == 0) color = &conf->colors.fg;
    else if (strcmp(key, "background") == 0) color = &conf->colors.bg;
    else if (strcmp(key, "selection-foreground") == 0) color = &conf->colors.selection_fg;
    else if (strcmp(key, "selection-background") == 0) color = &conf->colors.selection_bg;

    else if (strcmp(key, "jump-labels") == 0) {
        if (!value_to_two_colors(
                ctx,
                &conf->colors.jump_label.fg,
                &conf->colors.jump_label.bg,
                false))
        {
            return false;
        }

        conf->colors.use_custom.jump_label = true;
        return true;
    }

    else if (strcmp(key, "scrollback-indicator") == 0) {
        if (!value_to_two_colors(
                ctx,
                &conf->colors.scrollback_indicator.fg,
                &conf->colors.scrollback_indicator.bg,
                false))
        {
            return false;
        }

        conf->colors.use_custom.scrollback_indicator = true;
        return true;
    }

    else if (strcmp(key, "urls") == 0) {
        if (!value_to_color(ctx, &conf->colors.url, false))
            return false;

        conf->colors.use_custom.url = true;
        return true;
    }

    else if (strcmp(key, "alpha") == 0) {
        float alpha;
        if (!value_to_double(ctx, &alpha))
            return false;

        if (alpha < 0. || alpha > 1.) {
            LOG_CONTEXTUAL_ERR("not in range 0.0-1.0");
            return false;
        }

        conf->colors.alpha = alpha * 65535.;
        return true;
    }

    else {
        LOG_CONTEXTUAL_ERR("not valid option");
        return false;
    }

    uint32_t color_value;
    if (!value_to_color(ctx, &color_value, false))
        return false;

    *color = color_value;
    return true;
}

static bool
parse_section_cursor(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "style") == 0) {
        _Static_assert(sizeof(conf->cursor.style) == sizeof(int),
                       "enum is not 32-bit");

        return value_to_enum(
            ctx,
            (const char *[]){"block", "underline", "beam", NULL},
            (int *)&conf->cursor.style);
    }

    else if (strcmp(key, "blink") == 0)
        return value_to_bool(ctx, &conf->cursor.blink);

    else if (strcmp(key, "color") == 0) {
        if (!value_to_two_colors(
                ctx,
                &conf->cursor.color.text,
                &conf->cursor.color.cursor,
                false))
        {
            return false;
        }

        conf->cursor.color.text |= 1u << 31;
        conf->cursor.color.cursor |= 1u << 31;
        return true;
    }

    else if (strcmp(key, "beam-thickness") == 0)
        return value_to_pt_or_px(ctx, &conf->cursor.beam_thickness);

    else if (strcmp(key, "underline-thickness") == 0)
        return value_to_pt_or_px(ctx, &conf->cursor.underline_thickness);

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }
}

static bool
parse_section_mouse(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "hide-when-typing") == 0)
        return value_to_bool(ctx, &conf->mouse.hide_when_typing);

    else if (strcmp(key, "alternate-scroll-mode") == 0)
        return value_to_bool(ctx, &conf->mouse.alternate_scroll_mode);

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }
}

static bool
parse_section_csd(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "preferred") == 0) {
        _Static_assert(sizeof(conf->csd.preferred) == sizeof(int),
                       "enum is not 32-bit");

        return value_to_enum(
            ctx,
            (const char *[]){"none", "server", "client", NULL},
            (int *)&conf->csd.preferred);
    }

    else if (strcmp(key, "font") == 0) {
        struct config_font_list new_list = value_to_fonts(ctx);
        if (new_list.arr == NULL)
            return false;

        config_font_list_destroy(&conf->csd.font);
        conf->csd.font = new_list;
        return true;
    }

    else if (strcmp(key, "color") == 0) {
        uint32_t color;
        if (!value_to_color(ctx, &color, true))
            return false;

        conf->csd.color.title_set = true;
        conf->csd.color.title = color;
        return true;
    }

    else if (strcmp(key, "size") == 0)
        return value_to_uint16(ctx, 10, &conf->csd.title_height);

    else if (strcmp(key, "button-width") == 0)
        return value_to_uint16(ctx, 10, &conf->csd.button_width);

    else if (strcmp(key, "button-color") == 0) {
        if (!value_to_color(ctx, &conf->csd.color.buttons, true))
            return false;

        conf->csd.color.buttons_set = true;
        return true;
    }

    else if (strcmp(key, "button-minimize-color") == 0) {
        if (!value_to_color(ctx, &conf->csd.color.minimize, true))
            return false;

        conf->csd.color.minimize_set = true;
        return true;
    }

    else if (strcmp(key, "button-maximize-color") == 0) {
        if (!value_to_color(ctx, &conf->csd.color.maximize, true))
            return false;

        conf->csd.color.maximize_set = true;
        return true;
    }

    else if (strcmp(key, "button-close-color") == 0) {
        if (!value_to_color(ctx, &conf->csd.color.quit, true))
            return false;

        conf->csd.color.close_set = true;
        return true;
    }

    else if (strcmp(key, "border-color") == 0) {
        if (!value_to_color(ctx, &conf->csd.color.border, true))
            return false;

        conf->csd.color.border_set = true;
        return true;
    }

    else if (strcmp(key, "border-width") == 0)
        return value_to_uint16(ctx, 10, &conf->csd.border_width_visible);

    else if (strcmp(key, "hide-when-maximized") == 0)
        return value_to_bool(ctx, &conf->csd.hide_when_maximized);

    else {
        LOG_CONTEXTUAL_ERR("not a valid action: %s", key);
        return false;
    }
}

static void
free_binding_aux(struct binding_aux *aux)
{
    switch (aux->type) {
    case BINDING_AUX_NONE: break;
    case BINDING_AUX_PIPE: free_argv(&aux->pipe); break;
    case BINDING_AUX_TEXT: free(aux->text.data); break;
    }
}

static void
free_key_binding(struct config_key_binding *binding)
{
    free_binding_aux(&binding->aux);
}

static void NOINLINE
free_key_binding_list(struct config_key_binding_list *bindings)
{
    struct config_key_binding *binding = &bindings->arr[0];

    for (size_t i = 0; i < bindings->count; i++, binding++)
        free_key_binding(binding);
    free(bindings->arr);

    bindings->arr = NULL;
    bindings->count = 0;
}

static bool NOINLINE
parse_modifiers(struct context *ctx, const char *text, size_t len,
                struct config_key_modifiers *modifiers)
{
    bool ret = false;

    *modifiers = (struct config_key_modifiers){0};

    /* Handle "none" separately because e.g. none+shift is nonsense */
    if (strncmp(text, "none", len) == 0)
        return true;

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
            modifiers->super = true;
        else {
            LOG_CONTEXTUAL_ERR("not a valid modifier name: %s", key);
            goto out;
        }
    }

    ret = true;

out:
    free(copy);
    return ret;
}

static int NOINLINE
argv_compare(const struct argv *argv1, const struct argv *argv2)
{
    if (argv1->args == NULL && argv2->args == NULL)
        return 0;

    if (argv1->args == NULL)
        return -1;
    if (argv2->args == NULL)
        return 1;

    for (size_t i = 0; ; i++) {
        if (argv1->args[i] == NULL && argv2->args[i] == NULL)
            return 0;
        if (argv1->args[i] == NULL)
            return -1;
        if (argv2->args[i] == NULL)
            return 1;

        int ret = strcmp(argv1->args[i], argv2->args[i]);
        if (ret != 0)
            return ret;
    }

    BUG("unexpected loop break");
    return 1;
}

static bool NOINLINE
binding_aux_equal(const struct binding_aux *a,
                  const struct binding_aux *b)
{
    if (a->type != b->type)
        return false;

    switch (a->type) {
    case BINDING_AUX_NONE:
        return true;

    case BINDING_AUX_PIPE:
        return argv_compare(&a->pipe, &b->pipe) == 0;

    case BINDING_AUX_TEXT:
        return a->text.len == b->text.len &&
            memcmp(a->text.data, b->text.data, a->text.len) == 0;
    }

    BUG("invalid AUX type: %d", a->type);
    return false;
}

static void NOINLINE
remove_from_key_bindings_list(struct config_key_binding_list *bindings,
                              int action, const struct binding_aux *aux)
{
    size_t remove_first_idx = 0;
    size_t remove_count = 0;

    for (size_t i = 0; i < bindings->count; i++) {
        struct config_key_binding *binding = &bindings->arr[i];

        if (binding->action != action)
            continue;

        if (binding_aux_equal(&binding->aux, aux)) {
            if (remove_count++ == 0)
                remove_first_idx = i;

            xassert(remove_first_idx + remove_count - 1 == i);
            free_key_binding(binding);
        }
    }

    if (remove_count == 0)
        return;

    size_t move_count = bindings->count - (remove_first_idx + remove_count);

    memmove(
        &bindings->arr[remove_first_idx],
        &bindings->arr[remove_first_idx + remove_count],
        move_count * sizeof(bindings->arr[0]));
    bindings->count -= remove_count;
}

static const struct {
    const char *name;
    int code;
} button_map[] = {
    {"BTN_LEFT", BTN_LEFT},
    {"BTN_RIGHT", BTN_RIGHT},
    {"BTN_MIDDLE", BTN_MIDDLE},
    {"BTN_SIDE", BTN_SIDE},
    {"BTN_EXTRA", BTN_EXTRA},
    {"BTN_FORWARD", BTN_FORWARD},
    {"BTN_BACK", BTN_BACK},
    {"BTN_TASK", BTN_TASK},
};

static int
mouse_button_name_to_code(const char *name)
{
    for (size_t i = 0; i < ALEN(button_map); i++) {
        if (strcmp(button_map[i].name, name) == 0)
            return button_map[i].code;
    }
    return -1;
}

static const char*
mouse_button_code_to_name(int code)
{
    for (size_t i = 0; i < ALEN(button_map); i++) {
        if (code == button_map[i].code)
            return button_map[i].name;
    }

    return NULL;
}

static bool NOINLINE
value_to_key_combos(struct context *ctx, int action,
                    struct binding_aux *aux,
                    struct config_key_binding_list *bindings,
                    enum key_binding_type type)
{
    if (strcasecmp(ctx->value, "none") == 0) {
        remove_from_key_bindings_list(bindings, action, aux);
        return true;
    }

    /* Count number of combinations */
    size_t combo_count = 1;
    for (const char *p = strchr(ctx->value, ' ');
         p != NULL;
         p = strchr(p + 1, ' '))
    {
        combo_count++;
    }

    struct config_key_binding new_combos[combo_count];

    char *copy = xstrdup(ctx->value);
    size_t idx = 0;

    for (char *tok_ctx = NULL, *combo = strtok_r(copy, " ", &tok_ctx);
         combo != NULL;
         combo = strtok_r(NULL, " ", &tok_ctx),
             idx++)
    {
        struct config_key_binding *new_combo = &new_combos[idx];
        new_combo->action = action;
        new_combo->aux = *aux;
        new_combo->aux.master_copy = idx == 0;
#if 0
        new_combo->aux.type = BINDING_AUX_PIPE;
        new_combo->aux.master_copy = idx == 0;
        new_combo->aux.pipe = *argv;
#endif
        new_combo->path = ctx->path;
        new_combo->lineno = ctx->lineno;

        char *key = strrchr(combo, '+');

        if (key == NULL) {
            /* No modifiers */
            key = combo;
            new_combo->modifiers = (struct config_key_modifiers){0};
        } else {
            *key = '\0';
            if (!parse_modifiers(ctx, combo, key - combo, &new_combo->modifiers))
                goto err;
            key++;  /* Skip past the '+' */
        }

        switch (type) {
        case KEY_BINDING:
            /* Translate key name to symbol */
            new_combo->k.sym = xkb_keysym_from_name(key, 0);
            if (new_combo->k.sym == XKB_KEY_NoSymbol) {
                LOG_CONTEXTUAL_ERR("not a valid XKB key name: %s", key);
                goto err;
            }
            break;

        case MOUSE_BINDING: {
            new_combo->m.count = 1;

            char *_count = strrchr(key, '-');
            if (_count != NULL) {
                *_count = '\0';
                _count++;

                errno = 0;
                char *end;
                unsigned long value = strtoul(_count, &end, 10);
                if (_count[0] == '\0' || *end != '\0' || errno != 0) {
                    if (errno != 0)
                        LOG_CONTEXTUAL_ERRNO("invalid click count: %s", _count);
                    else
                        LOG_CONTEXTUAL_ERR("invalid click count: %s", _count);
                    goto err;
                }

                new_combo->m.count = value;
            }

            new_combo->m.button = mouse_button_name_to_code(key);
            if (new_combo->m.button < 0) {
                LOG_CONTEXTUAL_ERR("invalid mouse button name: %s", key);
                goto err;
            }

            break;
        }
        }

    }

    if (idx == 0) {
        LOG_CONTEXTUAL_ERR(
            "empty binding not allowed (set to 'none' to unmap)");
        goto err;
    }

    remove_from_key_bindings_list(bindings, action, aux);

    bindings->arr = xrealloc(
        bindings->arr,
        (bindings->count + combo_count) * sizeof(bindings->arr[0]));

    memcpy(&bindings->arr[bindings->count],
           new_combos,
           combo_count * sizeof(bindings->arr[0]));
    bindings->count += combo_count;

    free(copy);
    return true;

err:
    free(copy);
    return false;
}

static bool
modifiers_equal(const struct config_key_modifiers *mods1,
                const struct config_key_modifiers *mods2)
{
    bool shift = mods1->shift == mods2->shift;
    bool alt = mods1->alt == mods2->alt;
    bool ctrl = mods1->ctrl == mods2->ctrl;
    bool super = mods1->super == mods2->super;
    return shift && alt && ctrl && super;
}

static bool
modifiers_disjoint(const struct config_key_modifiers *mods1,
                const struct config_key_modifiers *mods2)
{
    bool shift = mods1->shift && mods2->shift;
    bool alt = mods1->alt && mods2->alt;
    bool ctrl = mods1->ctrl && mods2->ctrl;
    bool super = mods1->super && mods2->super;
    return !(shift || alt || ctrl || super);
}

static char * NOINLINE
modifiers_to_str(const struct config_key_modifiers *mods)
{
    char *ret = xasprintf(
        "%s%s%s%s",
        mods->ctrl ? XKB_MOD_NAME_CTRL "+" : "",
        mods->alt ? XKB_MOD_NAME_ALT "+": "",
        mods->super ? XKB_MOD_NAME_LOGO "+": "",
        mods->shift ? XKB_MOD_NAME_SHIFT "+": "");
    return ret;
}

/*
 * Parses a key binding value in the form
 *  "[cmd-to-exec arg1 arg2] Mods+Key"
 *
 * and extracts 'cmd-to-exec' and its arguments.
 *
 * Input:
 *  - value: raw string, in the form mentioned above
 *  - cmd: pointer to string to will be allocated and filled with
 *        'cmd-to-exec arg1 arg2'
 *  - argv: point to array of string. Array will be allocated. Will be
 *          filled with {'cmd-to-exec', 'arg1', 'arg2', NULL}
 *
 * Returns:
 *  - ssize_t, number of bytes that were stripped from 'value' to remove the '[]'
 *    enclosed cmd and its arguments, including any subsequent
 *    whitespace characters. I.e. if 'value' is "[cmd] BTN_RIGHT", the
 *    return value is 6 (strlen("[cmd] ")).
 *  - cmd: allocated string containing "cmd arg1 arg2...". Caller frees.
 *  - argv: allocated array containing {"cmd", "arg1", "arg2", NULL}. Caller frees.
 */
static ssize_t NOINLINE
pipe_argv_from_value(struct context *ctx, struct argv *argv)
{
    argv->args = NULL;

    if (ctx->value[0] != '[')
        return 0;

    const char *pipe_cmd_end = strrchr(ctx->value, ']');
    if (pipe_cmd_end == NULL) {
        LOG_CONTEXTUAL_ERR("unclosed '['");
        return -1;
    }

    size_t pipe_len = pipe_cmd_end - ctx->value - 1;
    char *cmd = xstrndup(&ctx->value[1], pipe_len);

    if (!tokenize_cmdline(cmd, &argv->args)) {
        LOG_CONTEXTUAL_ERR("syntax error in command line");
        free(cmd);
        return -1;
    }

    ssize_t remove_len = pipe_cmd_end + 1 - ctx->value;
    ctx->value = pipe_cmd_end + 1;
    while (isspace(*ctx->value)) {
        ctx->value++;
        remove_len++;
    }

    free(cmd);
    return remove_len;
}

static bool NOINLINE
parse_key_binding_section(struct context *ctx,
                          int action_count,
                          const char *const action_map[static action_count],
                          struct config_key_binding_list *bindings)
{
    struct binding_aux aux;

    ssize_t pipe_remove_len = pipe_argv_from_value(ctx, &aux.pipe);
    if (pipe_remove_len < 0)
        return false;

    aux.type = pipe_remove_len == 0 ? BINDING_AUX_NONE : BINDING_AUX_PIPE;
    aux.master_copy = true;

    for (int action = 0; action < action_count; action++) {
        if (action_map[action] == NULL)
            continue;

        if (strcmp(ctx->key, action_map[action]) != 0)
            continue;

        if (!value_to_key_combos(ctx, action, &aux, bindings, KEY_BINDING)) {
            free_binding_aux(&aux);
            return false;
        }

        return true;
    }

    LOG_CONTEXTUAL_ERR("not a valid action: %s", ctx->key);
    free_binding_aux(&aux);
    return false;
}

UNITTEST
{
    enum test_actions {
        TEST_ACTION_NONE,
        TEST_ACTION_FOO,
        TEST_ACTION_BAR,
        TEST_ACTION_COUNT,
    };

    const char *const map[] = {
        [TEST_ACTION_NONE] = NULL,
        [TEST_ACTION_FOO] = "foo",
        [TEST_ACTION_BAR] = "bar",
    };

    struct config conf = {0};
    struct config_key_binding_list bindings = {0};

    struct context ctx = {
        .conf = &conf,
        .section = "",
        .key = "foo",
        .value = "Escape",
        .path = "",
    };

    /*
     * ADD foo=Escape
     *
     * This verifies we can bind a single key combo to an action.
     */
    xassert(parse_key_binding_section(&ctx, ALEN(map), map, &bindings));
    xassert(bindings.count == 1);
    xassert(bindings.arr[0].action == TEST_ACTION_FOO);
    xassert(bindings.arr[0].k.sym == XKB_KEY_Escape);

    /*
     * ADD bar=Control+g Control+Shift+x
     *
     * This verifies we can bind multiple key combos to an action.
     */
    ctx.key = "bar";
    ctx.value = "Control+g Control+Shift+x";
    xassert(parse_key_binding_section(&ctx, ALEN(map), map, &bindings));
    xassert(bindings.count == 3);
    xassert(bindings.arr[0].action == TEST_ACTION_FOO);
    xassert(bindings.arr[1].action == TEST_ACTION_BAR);
    xassert(bindings.arr[1].k.sym == XKB_KEY_g);
    xassert(bindings.arr[1].modifiers.ctrl);
    xassert(bindings.arr[2].action == TEST_ACTION_BAR);
    xassert(bindings.arr[2].k.sym == XKB_KEY_x);
    xassert(bindings.arr[2].modifiers.ctrl && bindings.arr[2].modifiers.shift);

    /*
     * REPLACE foo with foo=Mod+v Shift+q
     *
     * This verifies we can update a single-combo action with multiple
     * key combos.
     */
    ctx.key = "foo";
    ctx.value = "Mod1+v Shift+q";
    xassert(parse_key_binding_section(&ctx, ALEN(map), map, &bindings));
    xassert(bindings.count == 4);
    xassert(bindings.arr[0].action == TEST_ACTION_BAR);
    xassert(bindings.arr[1].action == TEST_ACTION_BAR);
    xassert(bindings.arr[2].action == TEST_ACTION_FOO);
    xassert(bindings.arr[2].k.sym == XKB_KEY_v);
    xassert(bindings.arr[2].modifiers.alt);
    xassert(bindings.arr[3].action == TEST_ACTION_FOO);
    xassert(bindings.arr[3].k.sym == XKB_KEY_q);
    xassert(bindings.arr[3].modifiers.shift);

    /*
     * REMOVE bar
     */
    ctx.key = "bar";
    ctx.value = "none";
    xassert(parse_key_binding_section(&ctx, ALEN(map), map, &bindings));
    xassert(bindings.count == 2);
    xassert(bindings.arr[0].action == TEST_ACTION_FOO);
    xassert(bindings.arr[1].action == TEST_ACTION_FOO);

    /*
     * REMOVE foo
     */
    ctx.key = "foo";
    ctx.value = "none";
    xassert(parse_key_binding_section(&ctx, ALEN(map), map, &bindings));
    xassert(bindings.count == 0);

    free(bindings.arr);
}

static bool
parse_section_key_bindings(struct context *ctx)
{
    return parse_key_binding_section(
        ctx,
        BIND_ACTION_KEY_COUNT, binding_action_map,
        &ctx->conf->bindings.key);
}

static bool
parse_section_search_bindings(struct context *ctx)
{
    return parse_key_binding_section(
        ctx,
        BIND_ACTION_SEARCH_COUNT, search_binding_action_map,
        &ctx->conf->bindings.search);
}

static bool
parse_section_url_bindings(struct context *ctx)
{
    return parse_key_binding_section(
        ctx,
        BIND_ACTION_URL_COUNT, url_binding_action_map,
        &ctx->conf->bindings.url);
}

static bool NOINLINE
resolve_key_binding_collisions(struct config *conf, const char *section_name,
                               const char *const action_map[],
                               struct config_key_binding_list *bindings,
                               enum key_binding_type type)
{
    bool ret = true;

    for (size_t i = 1; i < bindings->count; i++) {
        enum {COLLISION_NONE,
            COLLISION_OVERRIDE,
            COLLISION_BINDING} collision_type = COLLISION_NONE;
        const struct config_key_binding *collision_binding = NULL;

        struct config_key_binding *binding1 = &bindings->arr[i];
        xassert(binding1->action != BIND_ACTION_NONE);

        const struct config_key_modifiers *mods1 = &binding1->modifiers;

        /* Does our modifiers collide with the selection override mods? */
        if (type == MOUSE_BINDING &&
            !modifiers_disjoint(
                mods1, &conf->mouse.selection_override_modifiers))
        {
            collision_type = COLLISION_OVERRIDE;
        }

        /* Does our binding collide with another binding? */
        for (ssize_t j = i - 1;
             collision_type == COLLISION_NONE && j >= 0;
             j--)
        {
            const struct config_key_binding *binding2 = &bindings->arr[j];
            xassert(binding2->action != BIND_ACTION_NONE);

            if (binding2->action == binding1->action &&
                binding_aux_equal(&binding1->aux, &binding2->aux))
            {
                continue;
            }

            const struct config_key_modifiers *mods2 = &binding2->modifiers;

            bool mods_equal = modifiers_equal(mods1, mods2);
            bool sym_equal;

            switch (type) {
            case KEY_BINDING:
                sym_equal = binding1->k.sym == binding2->k.sym;
                break;

            case MOUSE_BINDING:
                sym_equal = (binding1->m.button == binding2->m.button &&
                             binding1->m.count == binding2->m.count);
                break;
            }

            if (!mods_equal || !sym_equal)
                continue;

            collision_binding = binding2;
            collision_type = COLLISION_BINDING;
            break;
        }

        if (collision_type != COLLISION_NONE) {
            char *modifier_names = modifiers_to_str(mods1);
            char sym_name[64];

            switch (type){
            case KEY_BINDING:
                xkb_keysym_get_name(binding1->k.sym, sym_name, sizeof(sym_name));
                break;

            case MOUSE_BINDING: {
                const char *button_name =
                    mouse_button_code_to_name(binding1->m.button);

                if (binding1->m.count > 1) {
                    snprintf(sym_name, sizeof(sym_name), "%s-%d",
                             button_name, binding1->m.count);
                } else
                    strcpy(sym_name, button_name);
                break;
            }
            }

            switch (collision_type) {
            case COLLISION_NONE:
                break;

            case COLLISION_BINDING: {
                bool has_pipe = collision_binding->aux.type == BINDING_AUX_PIPE;
                LOG_AND_NOTIFY_ERR(
                    "%s:%d: [%s].%s: %s%s already mapped to '%s%s%s%s'",
                    binding1->path, binding1->lineno, section_name,
                    action_map[binding1->action],
                    modifier_names, sym_name,
                    action_map[collision_binding->action],
                    has_pipe ? " [" : "",
                    has_pipe ? collision_binding->aux.pipe.args[0] : "",
                    has_pipe ? "]" : "");
                ret = false;
                break;
            }

            case COLLISION_OVERRIDE: {
                char *override_names = modifiers_to_str(
                    &conf->mouse.selection_override_modifiers);

                if (override_names[0] != '\0')
                    override_names[strlen(override_names) - 1] = '\0';

                LOG_AND_NOTIFY_ERR(
                    "%s:%d: [%s].%s: %s%s: "
                    "modifiers conflict with 'selection-override-modifiers=%s'",
                    binding1->path != NULL ? binding1->path : "(default)",
                    binding1->lineno, section_name,
                    action_map[binding1->action],
                    modifier_names, sym_name, override_names);
                ret = false;
                free(override_names);
                break;
            }
            }

            free(modifier_names);

            if (binding1->aux.master_copy && i + 1 < bindings->count) {
                struct config_key_binding *next = &bindings->arr[i + 1];

                if (next->action == binding1->action &&
                    binding_aux_equal(&binding1->aux, &next->aux))
                {
                    /* Transfer ownership to next binding */
                    next->aux.master_copy = true;
                    binding1->aux.master_copy = false;
                }
            }

            free_key_binding(binding1);

            /* Remove the most recent binding */
            size_t move_count = bindings->count - (i + 1);
            memmove(&bindings->arr[i], &bindings->arr[i + 1],
                    move_count * sizeof(bindings->arr[0]));
            bindings->count--;

            i--;
        }
    }

    return ret;
}

static bool
parse_section_mouse_bindings(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;
    const char *value = ctx->value;

    if (strcmp(key, "selection-override-modifiers") == 0) {
        if (!parse_modifiers(
                ctx, ctx->value, strlen(value),
                &conf->mouse.selection_override_modifiers))
        {
            LOG_CONTEXTUAL_ERR("%s: invalid modifiers '%s'", key, ctx->value);
            return false;
        }
        return true;
    }

    struct binding_aux aux;

    ssize_t pipe_remove_len = pipe_argv_from_value(ctx, &aux.pipe);
    if (pipe_remove_len < 0)
        return false;

    aux.type = pipe_remove_len == 0 ? BINDING_AUX_NONE : BINDING_AUX_PIPE;
    aux.master_copy = true;

    for (enum bind_action_normal action = 0;
         action < BIND_ACTION_COUNT;
         action++)
    {
        if (binding_action_map[action] == NULL)
            continue;

        if (strcmp(key, binding_action_map[action]) != 0)
            continue;

        if (!value_to_key_combos(
                ctx, action, &aux, &conf->bindings.mouse, MOUSE_BINDING))
        {
            free_binding_aux(&aux);
            return false;
        }

        return true;
    }

    LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
    free_binding_aux(&aux);
    return false;
}

static bool
parse_section_text_bindings(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    const size_t key_len = strlen(key);

    uint8_t *data = xmalloc(key_len + 1);
    size_t data_len = 0;
    bool esc = false;

    for (size_t i = 0; i < key_len; i++) {
        if (key[i] == '\\') {
            if (i + 1 >= key_len) {
                ctx->value = "";
                LOG_CONTEXTUAL_ERR("trailing backslash");
                goto err;
            }

            esc = true;
        }

        else if (esc) {
            if (key[i] != 'x') {
                ctx->value = "";
                LOG_CONTEXTUAL_ERR("invalid escaped character: %c", key[i]);
                goto err;
            }
            if (i + 2 >= key_len) {
                ctx->value = "";
                LOG_CONTEXTUAL_ERR("\\x sequence too short");
                goto err;
            }

            const uint8_t nib1 = hex2nibble(key[i + 1]);
            const uint8_t nib2 = hex2nibble(key[i + 2]);

            if (nib1 >= HEX_DIGIT_INVALID || nib2 >= HEX_DIGIT_INVALID) {
                ctx->value = "";
                LOG_CONTEXTUAL_ERR("invalid \\x sequence: \\x%c%c",
                                   key[i + 1], key[i + 2]);
                goto err;
            }

            data[data_len++] = nib1 << 4 | nib2;
            esc = false;
            i += 2;
        }

        else
            data[data_len++] = key[i];
    }

    struct binding_aux aux = {
        .type = BINDING_AUX_TEXT,
        .text = {
            .data = data,
            .len = data_len,
        },
    };

    if (!value_to_key_combos(ctx, BIND_ACTION_TEXT_BINDING, &aux,
                             &conf->bindings.key, KEY_BINDING))
    {
        goto err;
    }

    return true;

err:
    free(data);
    return false;
}

static bool
parse_section_environment(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;
    const char *value = ctx->value;

    tll_foreach(conf->env_vars, it) {
        if (strcmp(it->item.name, key) == 0) {
            free(it->item.value);
            it->item.value = xstrdup(value);
            return true;
        }
    }

    struct env_var var = {
        .name = xstrdup(key),
        .value = xstrdup(value),
    };
    tll_push_back(conf->env_vars, var);
    return true;
}

static bool
parse_section_tweak(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "scaling-filter") == 0) {
        static const char *filters[] = {
            [FCFT_SCALING_FILTER_NONE] = "none",
            [FCFT_SCALING_FILTER_NEAREST] = "nearest",
            [FCFT_SCALING_FILTER_BILINEAR] = "bilinear",
            [FCFT_SCALING_FILTER_CUBIC] = "cubic",
            [FCFT_SCALING_FILTER_LANCZOS3] = "lanczos3",
            NULL,
        };

        _Static_assert(sizeof(conf->tweak.fcft_filter) == sizeof(int),
                       "enum is not 32-bit");

        return value_to_enum(ctx, filters, (int *)&conf->tweak.fcft_filter);
    }

    else if (strcmp(key, "overflowing-glyphs") == 0)
        return value_to_bool(ctx, &conf->tweak.overflowing_glyphs);

    else if (strcmp(key, "damage-whole-window") == 0)
        return value_to_bool(ctx, &conf->tweak.damage_whole_window);

    else if (strcmp(key, "grapheme-shaping") == 0) {
        if (!value_to_bool(ctx, &conf->tweak.grapheme_shaping))
            return false;

#if !defined(FOOT_GRAPHEME_CLUSTERING)
        if (conf->tweak.grapheme_shaping) {
            LOG_CONTEXTUAL_WARN(
                "foot was not compiled with support for grapheme shaping");
            conf->tweak.grapheme_shaping = false;
        }
#endif

        if (conf->tweak.grapheme_shaping && !conf->can_shape_grapheme) {
            LOG_WARN(
                "fcft was not compiled with support for grapheme shaping");

            /* Keep it enabled though - this will cause us to do
             * grapheme-clustering at least */
        }

        return true;
    }

    else if (strcmp(key, "grapheme-width-method") == 0) {
        _Static_assert(sizeof(conf->tweak.grapheme_width_method) == sizeof(int),
                       "enum is not 32-bit");

        return value_to_enum(
            ctx,
            (const char *[]){"wcswidth", "double-width", "max", NULL},
            (int *)&conf->tweak.grapheme_width_method);
    }

    else if (strcmp(key, "render-timer") == 0) {
        _Static_assert(sizeof(conf->tweak.render_timer) == sizeof(int),
                       "enum is not 32-bit");

        return value_to_enum(
            ctx,
            (const char *[]){"none", "osd", "log", "both", NULL},
            (int *)&conf->tweak.render_timer);
    }

    else if (strcmp(key, "delayed-render-lower") == 0) {
        uint32_t ns;
        if (!value_to_uint32(ctx, 10, &ns))
            return false;

        if (ns > 16666666) {
            LOG_CONTEXTUAL_ERR("timeout must not exceed 16ms");
            return false;
        }

        conf->tweak.delayed_render_lower_ns = ns;
        return true;
    }

    else if (strcmp(key, "delayed-render-upper") == 0) {
        uint32_t ns;
        if (!value_to_uint32(ctx, 10, &ns))
            return false;

        if (ns > 16666666) {
            LOG_CONTEXTUAL_ERR("timeout must not exceed 16ms");
            return false;
        }

        conf->tweak.delayed_render_upper_ns = ns;
        return true;
    }

    else if (strcmp(key, "max-shm-pool-size-mb") == 0) {
        uint32_t mb;
        if (!value_to_uint32(ctx, 10, &mb))
            return false;

        conf->tweak.max_shm_pool_size = min((int32_t)mb * 1024 * 1024, INT32_MAX);
        return true;
    }

    else if (strcmp(key, "box-drawing-base-thickness") == 0)
        return value_to_double(ctx, &conf->tweak.box_drawing_base_thickness);

    else if (strcmp(key, "box-drawing-solid-shades") == 0)
        return value_to_bool(ctx, &conf->tweak.box_drawing_solid_shades);

    else if (strcmp(key, "font-monospace-warn") == 0)
        return value_to_bool(ctx, &conf->tweak.font_monospace_warn);

    else if (strcmp(key, "sixel") == 0)
        return value_to_bool(ctx, &conf->tweak.sixel);

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }
}

static bool
parse_key_value(char *kv, const char **section, const char **key, const char **value)
{
    bool section_is_needed = section != NULL;

    /* Strip leading whitespace */
    while (isspace(kv[0]))
        ++kv;

    if (section_is_needed)
        *section = "main";

    if (kv[0] == '=')
        return false;

    *key = kv;
    *value = NULL;

    size_t kvlen = strlen(kv);

    /* Strip trailing whitespace */
    while (isspace(kv[kvlen - 1]))
        kvlen--;
    kv[kvlen] = '\0';

    for (size_t i = 0; i < kvlen; ++i) {
        if (kv[i] == '.' && section_is_needed) {
            section_is_needed = false;
            *section = kv;
            kv[i] = '\0';
            if (i == kvlen - 1 || kv[i + 1] == '=') {
                *key = NULL;
                return false;
            }
            *key = &kv[i + 1];
        } else if (kv[i] == '=') {
            kv[i] = '\0';
            if (i != kvlen - 1)
                *value = &kv[i + 1];
            break;
        }
    }

    if (*value == NULL)
        return false;

    /* Strip trailing whitespace from key (leading stripped earlier) */
    {
        xassert(!isspace(*key[0]));

        char *end = (char *)*key + strlen(*key) - 1;
        while (isspace(end[0]))
            end--;
        end[1] = '\0';
    }

    /* Strip leading whitespace from value (trailing stripped earlier) */
    while (isspace(*value[0]))
        ++*value;

    return true;
}

enum section {
    SECTION_MAIN,
    SECTION_BELL,
    SECTION_SCROLLBACK,
    SECTION_URL,
    SECTION_COLORS,
    SECTION_CURSOR,
    SECTION_MOUSE,
    SECTION_CSD,
    SECTION_KEY_BINDINGS,
    SECTION_SEARCH_BINDINGS,
    SECTION_URL_BINDINGS,
    SECTION_MOUSE_BINDINGS,
    SECTION_TEXT_BINDINGS,
    SECTION_ENVIRONMENT,
    SECTION_TWEAK,
    SECTION_COUNT,
};

/* Function pointer, called for each key/value line */
typedef bool (*parser_fun_t)(struct context *ctx);

static const struct {
    parser_fun_t fun;
    const char *name;
} section_info[] = {
    [SECTION_MAIN] =            {&parse_section_main, "main"},
    [SECTION_BELL] =            {&parse_section_bell, "bell"},
    [SECTION_SCROLLBACK] =      {&parse_section_scrollback, "scrollback"},
    [SECTION_URL] =             {&parse_section_url, "url"},
    [SECTION_COLORS] =          {&parse_section_colors, "colors"},
    [SECTION_CURSOR] =          {&parse_section_cursor, "cursor"},
    [SECTION_MOUSE] =           {&parse_section_mouse, "mouse"},
    [SECTION_CSD] =             {&parse_section_csd, "csd"},
    [SECTION_KEY_BINDINGS] =    {&parse_section_key_bindings, "key-bindings"},
    [SECTION_SEARCH_BINDINGS] = {&parse_section_search_bindings, "search-bindings"},
    [SECTION_URL_BINDINGS] =    {&parse_section_url_bindings, "url-bindings"},
    [SECTION_MOUSE_BINDINGS] =  {&parse_section_mouse_bindings, "mouse-bindings"},
    [SECTION_TEXT_BINDINGS] =   {&parse_section_text_bindings, "text-bindings"},
    [SECTION_ENVIRONMENT] =     {&parse_section_environment, "environment"},
    [SECTION_TWEAK] =           {&parse_section_tweak, "tweak"},
};

static_assert(ALEN(section_info) == SECTION_COUNT, "section info array size mismatch");

static enum section
str_to_section(const char *str)
{
    for (enum section section = SECTION_MAIN; section < SECTION_COUNT; ++section) {
        if (strcmp(str, section_info[section].name) == 0)
            return section;
    }
    return SECTION_COUNT;
}

static bool
parse_config_file(FILE *f, struct config *conf, const char *path, bool errors_are_fatal)
{
    enum section section = SECTION_MAIN;

    char *_line = NULL;
    size_t count = 0;
    bool ret = true;

#define error_or_continue()                     \
    {                                           \
        if (errors_are_fatal) {                 \
            ret = false;                        \
            goto done;                          \
        } else                                  \
            continue;                           \
    }

    char *section_name = xstrdup("main");

    struct context context = {
        .conf = conf,
        .section = section_name,
        .path = path,
        .lineno = 0,
        .errors_are_fatal = errors_are_fatal,
    };
    struct context *ctx = &context;  /* For LOG_AND_*() */

    errno = 0;
    ssize_t len;

    while ((len = getline(&_line, &count, f)) != -1) {
        context.key = NULL;
        context.value = NULL;
        context.lineno++;

        char *line = _line;

        /* Strip leading whitespace */
        while (isspace(line[0])) {
            line++;
            len--;
        }

        /* Empty line, or comment */
        if (line[0] == '\0' || line[0] == '#')
            continue;

        /* Strip the trailing newline - may be absent on the last line */
        if (line[len - 1] == '\n')
            line[--len] = '\0';

        /* Split up into key/value pair + trailing comment separated by blank */
        char *key_value = line;
        char *kv_trailing = &line[len - 1];
        char *comment = &line[1];
        while (comment[1] != '\0') {
            if (isblank(comment[0]) && comment[1] == '#') {
                comment[1] = '\0'; /* Terminate key/value pair */
                kv_trailing = comment++;
                break;
            }
            comment++;
        }
        comment++;

        /* Strip trailing whitespace */
        while (isspace(kv_trailing[0]))
            kv_trailing--;
        kv_trailing[1] = '\0';

        /* Check for new section */
        if (key_value[0] == '[') {
            key_value++;

            if (key_value[0] == ']') {
                LOG_CONTEXTUAL_ERR("empty section name");
                section = SECTION_COUNT;
                error_or_continue();
            }

            char *end = strchr(key_value, ']');

            if (end == NULL) {
                context.section = key_value;
                LOG_CONTEXTUAL_ERR("syntax error: no closing ']'");
                context.section = section_name;
                section = SECTION_COUNT;
                error_or_continue();
            }

            end[0] = '\0';

            if (end[1] != '\0') {
                context.section = key_value;
                LOG_CONTEXTUAL_ERR("section declaration contains trailing "
                                   "characters");
                context.section = section_name;
                section = SECTION_COUNT;
                error_or_continue();
            }

            section = str_to_section(key_value);
            if (section == SECTION_COUNT) {
                context.section = key_value;
                LOG_CONTEXTUAL_ERR("invalid section name: %s", key_value);
                context.section = section_name;
                error_or_continue();
            }

            free(section_name);
            section_name = xstrdup(key_value);
            context.section = section_name;

            /* Process next line */
            continue;
        }

        if (section >= SECTION_COUNT) {
            /* Last section name was invalid; ignore all keys in it */
            continue;
        }

        if (!parse_key_value(key_value, NULL, &context.key, &context.value)) {
            LOG_CONTEXTUAL_ERR("syntax error: key/value pair has no %s",
                               context.key == NULL ? "key" : "value");
            error_or_continue();
        }

        LOG_DBG("section=%s, key='%s', value='%s', comment='%s'",
                section_info[section].name, context.key, context.value, comment);

        xassert(section >= 0 && section < SECTION_COUNT);

        parser_fun_t section_parser = section_info[section].fun;
        xassert(section_parser != NULL);

        if (!section_parser(ctx))
            error_or_continue();
    }

    if (errno != 0) {
        LOG_AND_NOTIFY_ERRNO("failed to read from configuration");
        if (errors_are_fatal)
            ret = false;
    }

done:
    free(section_name);
    free(_line);
    return ret;
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

#define m_none       {0}
#define m_alt        {.alt = true}
#define m_ctrl       {.ctrl = true}
#define m_shift      {.shift = true}
#define m_ctrl_shift {.ctrl = true, .shift = true}

static void
add_default_key_bindings(struct config *conf)
{
    static const struct config_key_binding bindings[] = {
        {BIND_ACTION_SCROLLBACK_UP_PAGE, m_shift, {{XKB_KEY_Prior}}},
        {BIND_ACTION_SCROLLBACK_DOWN_PAGE, m_shift, {{XKB_KEY_Next}}},
        {BIND_ACTION_CLIPBOARD_COPY, m_ctrl_shift, {{XKB_KEY_c}}},
        {BIND_ACTION_CLIPBOARD_COPY, m_none, {{XKB_KEY_XF86Copy}}},
        {BIND_ACTION_CLIPBOARD_PASTE, m_ctrl_shift, {{XKB_KEY_v}}},
        {BIND_ACTION_CLIPBOARD_PASTE, m_none, {{XKB_KEY_XF86Paste}}},
        {BIND_ACTION_PRIMARY_PASTE, m_shift, {{XKB_KEY_Insert}}},
        {BIND_ACTION_SEARCH_START, m_ctrl_shift, {{XKB_KEY_r}}},
        {BIND_ACTION_FONT_SIZE_UP, m_ctrl, {{XKB_KEY_plus}}},
        {BIND_ACTION_FONT_SIZE_UP, m_ctrl, {{XKB_KEY_equal}}},
        {BIND_ACTION_FONT_SIZE_UP, m_ctrl, {{XKB_KEY_KP_Add}}},
        {BIND_ACTION_FONT_SIZE_DOWN, m_ctrl, {{XKB_KEY_minus}}},
        {BIND_ACTION_FONT_SIZE_DOWN, m_ctrl, {{XKB_KEY_KP_Subtract}}},
        {BIND_ACTION_FONT_SIZE_RESET, m_ctrl, {{XKB_KEY_0}}},
        {BIND_ACTION_FONT_SIZE_RESET, m_ctrl, {{XKB_KEY_KP_0}}},
        {BIND_ACTION_SPAWN_TERMINAL, m_ctrl_shift, {{XKB_KEY_n}}},
        {BIND_ACTION_SHOW_URLS_LAUNCH, m_ctrl_shift, {{XKB_KEY_u}}},
        {BIND_ACTION_PROMPT_PREV, m_ctrl_shift, {{XKB_KEY_z}}},
        {BIND_ACTION_PROMPT_NEXT, m_ctrl_shift, {{XKB_KEY_x}}},
    };

    conf->bindings.key.count = ALEN(bindings);
    conf->bindings.key.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.key.arr, bindings, sizeof(bindings));
}


static void
add_default_search_bindings(struct config *conf)
{
    static const struct config_key_binding bindings[] = {
        {BIND_ACTION_SEARCH_CANCEL, m_ctrl, {{XKB_KEY_c}}},
        {BIND_ACTION_SEARCH_CANCEL, m_ctrl, {{XKB_KEY_g}}},
        {BIND_ACTION_SEARCH_CANCEL, m_none, {{XKB_KEY_Escape}}},
        {BIND_ACTION_SEARCH_COMMIT, m_none, {{XKB_KEY_Return}}},
        {BIND_ACTION_SEARCH_FIND_PREV, m_ctrl, {{XKB_KEY_r}}},
        {BIND_ACTION_SEARCH_FIND_NEXT, m_ctrl, {{XKB_KEY_s}}},
        {BIND_ACTION_SEARCH_EDIT_LEFT, m_none, {{XKB_KEY_Left}}},
        {BIND_ACTION_SEARCH_EDIT_LEFT, m_ctrl, {{XKB_KEY_b}}},
        {BIND_ACTION_SEARCH_EDIT_LEFT_WORD, m_ctrl, {{XKB_KEY_Left}}},
        {BIND_ACTION_SEARCH_EDIT_LEFT_WORD, m_alt, {{XKB_KEY_b}}},
        {BIND_ACTION_SEARCH_EDIT_RIGHT, m_none, {{XKB_KEY_Right}}},
        {BIND_ACTION_SEARCH_EDIT_RIGHT, m_ctrl, {{XKB_KEY_f}}},
        {BIND_ACTION_SEARCH_EDIT_RIGHT_WORD, m_ctrl, {{XKB_KEY_Right}}},
        {BIND_ACTION_SEARCH_EDIT_RIGHT_WORD, m_alt, {{XKB_KEY_f}}},
        {BIND_ACTION_SEARCH_EDIT_HOME, m_none, {{XKB_KEY_Home}}},
        {BIND_ACTION_SEARCH_EDIT_HOME, m_ctrl, {{XKB_KEY_a}}},
        {BIND_ACTION_SEARCH_EDIT_END, m_none, {{XKB_KEY_End}}},
        {BIND_ACTION_SEARCH_EDIT_END, m_ctrl, {{XKB_KEY_e}}},
        {BIND_ACTION_SEARCH_DELETE_PREV, m_none, {{XKB_KEY_BackSpace}}},
        {BIND_ACTION_SEARCH_DELETE_PREV_WORD, m_ctrl, {{XKB_KEY_BackSpace}}},
        {BIND_ACTION_SEARCH_DELETE_PREV_WORD, m_alt, {{XKB_KEY_BackSpace}}},
        {BIND_ACTION_SEARCH_DELETE_NEXT, m_none, {{XKB_KEY_Delete}}},
        {BIND_ACTION_SEARCH_DELETE_NEXT_WORD, m_ctrl, {{XKB_KEY_Delete}}},
        {BIND_ACTION_SEARCH_DELETE_NEXT_WORD, m_alt, {{XKB_KEY_d}}},
        {BIND_ACTION_SEARCH_EXTEND_WORD, m_ctrl, {{XKB_KEY_w}}},
        {BIND_ACTION_SEARCH_EXTEND_WORD_WS, m_ctrl_shift, {{XKB_KEY_w}}},
        {BIND_ACTION_SEARCH_CLIPBOARD_PASTE, m_ctrl, {{XKB_KEY_v}}},
        {BIND_ACTION_SEARCH_CLIPBOARD_PASTE, m_ctrl_shift, {{XKB_KEY_v}}},
        {BIND_ACTION_SEARCH_CLIPBOARD_PASTE, m_ctrl, {{XKB_KEY_y}}},
        {BIND_ACTION_SEARCH_CLIPBOARD_PASTE, m_none, {{XKB_KEY_XF86Paste}}},
        {BIND_ACTION_SEARCH_PRIMARY_PASTE, m_shift, {{XKB_KEY_Insert}}},
    };

    conf->bindings.search.count = ALEN(bindings);
    conf->bindings.search.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.search.arr, bindings, sizeof(bindings));
}

static void
add_default_url_bindings(struct config *conf)
{
    static const struct config_key_binding bindings[] = {
        {BIND_ACTION_URL_CANCEL, m_ctrl, {{XKB_KEY_c}}},
        {BIND_ACTION_URL_CANCEL, m_ctrl, {{XKB_KEY_g}}},
        {BIND_ACTION_URL_CANCEL, m_ctrl, {{XKB_KEY_d}}},
        {BIND_ACTION_URL_CANCEL, m_none, {{XKB_KEY_Escape}}},
        {BIND_ACTION_URL_TOGGLE_URL_ON_JUMP_LABEL, m_none, {{XKB_KEY_t}}},
    };

    conf->bindings.url.count = ALEN(bindings);
    conf->bindings.url.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.url.arr, bindings, sizeof(bindings));
}

static void
add_default_mouse_bindings(struct config *conf)
{
    static const struct config_key_binding bindings[] = {
        {BIND_ACTION_PRIMARY_PASTE, m_none, {.m = {BTN_MIDDLE, 1}}},
        {BIND_ACTION_SELECT_BEGIN, m_none, {.m = {BTN_LEFT, 1}}},
        {BIND_ACTION_SELECT_BEGIN_BLOCK, m_ctrl, {.m = {BTN_LEFT, 1}}},
        {BIND_ACTION_SELECT_EXTEND, m_none, {.m = {BTN_RIGHT, 1}}},
        {BIND_ACTION_SELECT_EXTEND_CHAR_WISE, m_ctrl, {.m = {BTN_RIGHT, 1}}},
        {BIND_ACTION_SELECT_WORD, m_none, {.m = {BTN_LEFT, 2}}},
        {BIND_ACTION_SELECT_WORD_WS, m_ctrl, {.m = {BTN_LEFT, 2}}},
        {BIND_ACTION_SELECT_ROW, m_none, {.m = {BTN_LEFT, 3}}},
    };

    conf->bindings.mouse.count = ALEN(bindings);
    conf->bindings.mouse.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.mouse.arr, bindings, sizeof(bindings));
}

static void NOINLINE
config_font_list_clone(struct config_font_list *dst,
                       const struct config_font_list *src)
{
    dst->count = src->count;
    dst->arr = xmalloc(dst->count * sizeof(dst->arr[0]));

    for (size_t j = 0; j < dst->count; j++) {
        dst->arr[j].pt_size = src->arr[j].pt_size;
        dst->arr[j].px_size = src->arr[j].px_size;
        dst->arr[j].pattern = xstrdup(src->arr[j].pattern);
    }
}

bool
config_load(struct config *conf, const char *conf_path,
            user_notifications_t *initial_user_notifications,
            config_override_t *overrides, bool errors_are_fatal)
{
    bool ret = false;
    enum fcft_capabilities fcft_caps = fcft_capabilities();

    *conf = (struct config) {
        .term = xstrdup(FOOT_DEFAULT_TERM),
        .shell = get_shell(),
        .title = xstrdup("foot"),
        .app_id = xstrdup("foot"),
        .word_delimiters = xc32dup(U",│`|:\"'()[]{}<>"),
        .size = {
            .type = CONF_SIZE_PX,
            .width = 700,
            .height = 500,
        },
        .pad_x = 2,
        .pad_y = 2,
        .resize_delay_ms = 100,
        .bold_in_bright = {
            .enabled = false,
            .palette_based = false,
        },
        .startup_mode = STARTUP_WINDOWED,
        .fonts = {{0}},
        .line_height = {.pt = 0, .px = -1},
        .letter_spacing = {.pt = 0, .px = 0},
        .horizontal_letter_offset = {.pt = 0, .px = 0},
        .vertical_letter_offset = {.pt = 0, .px = 0},
        .use_custom_underline_offset = false,
        .box_drawings_uses_font_glyphs = false,
        .dpi_aware = DPI_AWARE_AUTO, /* DPI-aware when scaling-factor == 1 */
        .bell = {
            .urgent = false,
            .notify = false,
            .command = {
                .argv = {.args = NULL},
            },
            .command_focused = false,
        },
        .url = {
            .label_letters = xc32dup(U"sadfjklewcmpgh"),
            .uri_characters = xc32dup(U"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.,~:;/?#@!$&%*+=\"'()[]"),
            .osc8_underline = OSC8_UNDERLINE_URL_MODE,
        },
        .can_shape_grapheme = fcft_caps & FCFT_CAPABILITY_GRAPHEME_SHAPING,
        .scrollback = {
            .lines = 1000,
            .indicator = {
                .position = SCROLLBACK_INDICATOR_POSITION_RELATIVE,
                .format = SCROLLBACK_INDICATOR_FORMAT_TEXT,
                .text = xc32dup(U""),
            },
            .multiplier = 3.,
        },
        .colors = {
            .fg = default_foreground,
            .bg = default_background,
            .alpha = 0xffff,
            .selection_fg = 0x80000000,  /* Use default bg */
            .selection_bg = 0x80000000,  /* Use default fg */
            .use_custom = {
                .selection = false,
                .jump_label = false,
                .scrollback_indicator = false,
                .url = false,
            },
        },

        .cursor = {
            .style = CURSOR_BLOCK,
            .blink = false,
            .color = {
                .text = 0,
                .cursor = 0,
            },
            .beam_thickness = {.pt = 1.5},
            .underline_thickness = {.pt = 0., .px = -1},
        },
        .mouse = {
            .hide_when_typing = false,
            .alternate_scroll_mode = true,
            .selection_override_modifiers = {
                .shift = true,
                .alt = false,
                .ctrl = false,
                .super = false,
            },
        },
        .csd = {
            .preferred = CONF_CSD_PREFER_SERVER,
            .font = {0},
            .hide_when_maximized = false,
            .title_height = 26,
            .border_width = 5,
            .border_width_visible = 0,
            .button_width = 26,
        },

        .render_worker_count = sysconf(_SC_NPROCESSORS_ONLN),
        .server_socket_path = get_server_socket_path(),
        .presentation_timings = false,
        .selection_target = SELECTION_TARGET_PRIMARY,
        .hold_at_exit = false,
        .notify = {
            .argv = {.args = NULL},
        },
        .notify_focus_inhibit = true,

        .tweak = {
            .fcft_filter = FCFT_SCALING_FILTER_LANCZOS3,
            .overflowing_glyphs = true,
#if defined(FOOT_GRAPHEME_CLUSTERING) && FOOT_GRAPHEME_CLUSTERING
            .grapheme_shaping = fcft_caps & FCFT_CAPABILITY_GRAPHEME_SHAPING,
#endif
            .grapheme_width_method = GRAPHEME_WIDTH_WCSWIDTH,
            .delayed_render_lower_ns = 500000,         /* 0.5ms */
            .delayed_render_upper_ns = 16666666 / 2,   /* half a frame period (60Hz) */
            .max_shm_pool_size = 512 * 1024 * 1024,
            .render_timer = RENDER_TIMER_NONE,
            .damage_whole_window = false,
            .box_drawing_base_thickness = 0.04,
            .box_drawing_solid_shades = true,
            .font_monospace_warn = true,
            .sixel = true,
        },

        .env_vars = tll_init(),
        .notifications = tll_init(),
    };

    memcpy(conf->colors.table, default_color_table, sizeof(default_color_table));

    tokenize_cmdline("notify-send -a ${app-id} -i ${app-id} ${title} ${body}",
                     &conf->notify.argv.args);
    tokenize_cmdline("xdg-open ${url}", &conf->url.launch.argv.args);

    static const char32_t *url_protocols[] = {
        U"http://",
        U"https://",
        U"ftp://",
        U"ftps://",
        U"file://",
        U"gemini://",
        U"gopher://",
        U"irc://",
        U"ircs://",
    };
    conf->url.protocols = xmalloc(
        ALEN(url_protocols) * sizeof(conf->url.protocols[0]));
    conf->url.prot_count = ALEN(url_protocols);
    conf->url.max_prot_len = 0;

    for (size_t i = 0; i < ALEN(url_protocols); i++) {
        size_t len = c32len(url_protocols[i]);
        if (len > conf->url.max_prot_len)
            conf->url.max_prot_len = len;
        conf->url.protocols[i] = xc32dup(url_protocols[i]);
    }

    qsort(
        conf->url.uri_characters,
        c32len(conf->url.uri_characters),
        sizeof(conf->url.uri_characters[0]),
        &c32cmp_single);

    tll_foreach(*initial_user_notifications, it) {
        tll_push_back(conf->notifications, it->item);
        tll_remove(*initial_user_notifications, it);
    }

    add_default_key_bindings(conf);
    add_default_search_bindings(conf);
    add_default_url_bindings(conf);
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
        conf_file = open_config();
        if (conf_file.fd < 0) {
            LOG_WARN("no configuration found, using defaults");
            ret = !errors_are_fatal;
            goto out;
        }
    }

    xassert(conf_file.path != NULL);
    xassert(conf_file.fd >= 0);
    LOG_INFO("loading configuration from %s", conf_file.path);

    FILE *f = fdopen(conf_file.fd, "r");
    if (f == NULL) {
        LOG_AND_NOTIFY_ERRNO("%s: failed to open", conf_file.path);
        ret = !errors_are_fatal;
        goto out;
    }

    if (!parse_config_file(f, conf, conf_file.path, errors_are_fatal) ||
        !config_override_apply(conf, overrides, errors_are_fatal))
    {
        ret = !errors_are_fatal;
    } else
        ret = true;

    fclose(f);

    conf->colors.use_custom.selection =
        conf->colors.selection_fg >> 24 == 0 &&
        conf->colors.selection_bg >> 24 == 0;

out:
    if (ret && conf->fonts[0].count == 0) {
        struct config_font font;
        if (!config_font_parse("monospace", &font)) {
            LOG_ERR("failed to load font 'monospace' - no fonts installed?");
            ret = false;
        } else {
            conf->fonts[0].count = 1;
            conf->fonts[0].arr = malloc(sizeof(font));
            conf->fonts[0].arr[0] = font;
        }
    }

    if (ret && conf->csd.font.count == 0)
        config_font_list_clone(&conf->csd.font, &conf->fonts[0]);

#if defined(_DEBUG)
    for (size_t i = 0; i < conf->bindings.key.count; i++)
        xassert(conf->bindings.key.arr[i].action != BIND_ACTION_NONE);
    for (size_t i = 0; i < conf->bindings.search.count; i++)
        xassert(conf->bindings.search.arr[i].action != BIND_ACTION_SEARCH_NONE);
    for (size_t i = 0; i < conf->bindings.url.count; i++)
        xassert(conf->bindings.url.arr[i].action != BIND_ACTION_URL_NONE);
#endif

    free(conf_file.path);
    if (conf_file.fd >= 0)
        close(conf_file.fd);

    return ret;
}

bool
config_override_apply(struct config *conf, config_override_t *overrides,
                      bool errors_are_fatal)
{
    struct context context = {
        .conf = conf,
        .path = "override",
        .lineno = 0,
        .errors_are_fatal = errors_are_fatal,
    };
    struct context *ctx = &context;

    tll_foreach(*overrides, it) {
        context.lineno++;

        if (!parse_key_value(
                it->item, &context.section, &context.key, &context.value))
        {
            LOG_CONTEXTUAL_ERR("syntax error: key/value pair has no %s",
                               context.key == NULL ? "key" : "value");
            if (errors_are_fatal)
                return false;
            continue;
        }

        if (context.section[0] == '\0') {
            LOG_CONTEXTUAL_ERR("empty section name");
            if (errors_are_fatal)
                return false;
            continue;
        }

        enum section section = str_to_section(context.section);
        if (section == SECTION_COUNT) {
            LOG_CONTEXTUAL_ERR("invalid section name: %s", context.section);
            if (errors_are_fatal)
                return false;
            continue;
        }
        parser_fun_t section_parser = section_info[section].fun;
        xassert(section_parser != NULL);

        if (!section_parser(ctx)) {
            if (errors_are_fatal)
                return false;
            continue;
        }
    }

    conf->csd.border_width = max(
        min_csd_border_width, conf->csd.border_width_visible);

    return
        resolve_key_binding_collisions(
            conf, section_info[SECTION_KEY_BINDINGS].name,
            binding_action_map, &conf->bindings.key, KEY_BINDING) &&
        resolve_key_binding_collisions(
            conf, section_info[SECTION_SEARCH_BINDINGS].name,
            search_binding_action_map, &conf->bindings.search, KEY_BINDING) &&
        resolve_key_binding_collisions(
            conf, section_info[SECTION_URL_BINDINGS].name,
            url_binding_action_map, &conf->bindings.url, KEY_BINDING) &&
        resolve_key_binding_collisions(
            conf, section_info[SECTION_MOUSE_BINDINGS].name,
            binding_action_map, &conf->bindings.mouse, MOUSE_BINDING);
}

static void NOINLINE
key_binding_list_clone(struct config_key_binding_list *dst,
                       const struct config_key_binding_list *src)
{
    struct argv *last_master_argv = NULL;
    uint8_t *last_master_text_data = NULL;
    size_t last_master_text_len = 0;

    dst->count = src->count;
    dst->arr = xmalloc(src->count * sizeof(dst->arr[0]));

    for (size_t i = 0; i < src->count; i++) {
        const struct config_key_binding *old = &src->arr[i];
        struct config_key_binding *new = &dst->arr[i];

        *new = *old;

        switch (old->aux.type) {
        case BINDING_AUX_NONE:
            last_master_argv = NULL;
            last_master_text_data = NULL;
            last_master_text_len = 0;
            break;

        case BINDING_AUX_PIPE:
            if (old->aux.master_copy) {
                clone_argv(&new->aux.pipe, &old->aux.pipe);
                last_master_argv = &new->aux.pipe;
            } else {
                xassert(last_master_argv != NULL);
                new->aux.pipe = *last_master_argv;
            }
            last_master_text_data = NULL;
            last_master_text_len = 0;
            break;

        case BINDING_AUX_TEXT:
            if (old->aux.master_copy) {
                const size_t len = old->aux.text.len;
                new->aux.text.len = len;
                new->aux.text.data = xmalloc(len);
                memcpy(new->aux.text.data, old->aux.text.data, len);

                last_master_text_len = len;
                last_master_text_data = new->aux.text.data;
            } else {
                xassert(last_master_text_data != NULL);
                new->aux.text.len = last_master_text_len;
                new->aux.text.data = last_master_text_data;
            }
            last_master_argv = NULL;
            break;
        }
    }
}

struct config *
config_clone(const struct config *old)
{
    struct config *conf = xmalloc(sizeof(*conf));
    *conf = *old;

    conf->term = xstrdup(old->term);
    conf->shell = xstrdup(old->shell);
    conf->title = xstrdup(old->title);
    conf->app_id = xstrdup(old->app_id);
    conf->word_delimiters = xc32dup(old->word_delimiters);
    conf->scrollback.indicator.text = xc32dup(old->scrollback.indicator.text);
    conf->server_socket_path = xstrdup(old->server_socket_path);
    spawn_template_clone(&conf->bell.command, &old->bell.command);
    spawn_template_clone(&conf->notify, &old->notify);

    for (size_t i = 0; i < ALEN(conf->fonts); i++)
        config_font_list_clone(&conf->fonts[i], &old->fonts[i]);
    config_font_list_clone(&conf->csd.font, &old->csd.font);

    conf->url.label_letters = xc32dup(old->url.label_letters);
    conf->url.uri_characters = xc32dup(old->url.uri_characters);
    spawn_template_clone(&conf->url.launch, &old->url.launch);
    conf->url.protocols = xmalloc(
        old->url.prot_count * sizeof(conf->url.protocols[0]));
    for (size_t i = 0; i < old->url.prot_count; i++)
        conf->url.protocols[i] = xc32dup(old->url.protocols[i]);

    key_binding_list_clone(&conf->bindings.key, &old->bindings.key);
    key_binding_list_clone(&conf->bindings.search, &old->bindings.search);
    key_binding_list_clone(&conf->bindings.url, &old->bindings.url);
    key_binding_list_clone(&conf->bindings.mouse, &old->bindings.mouse);

    tll_foreach(old->env_vars, it) {
        struct env_var copy = {
            .name = xstrdup(it->item.name),
            .value = xstrdup(it->item.value),
        };
        tll_push_back(conf->env_vars, copy);
    }

    conf->notifications.length = 0;
    conf->notifications.head = conf->notifications.tail = 0;
    tll_foreach(old->notifications, it) {
        char *text = xstrdup(it->item.text);
        user_notification_add(&conf->notifications, it->item.kind, text);
    }

    return conf;
}

UNITTEST
{
    struct config original;
    user_notifications_t nots = tll_init();
    config_override_t overrides = tll_init();

    bool ret = config_load(&original, "/dev/null", &nots, &overrides, false);
    xassert(ret);

    struct config *clone = config_clone(&original);
    xassert(clone != NULL);
    xassert(clone != &original);

    config_free(&original);
    config_free(clone);
    free(clone);

    tll_free(overrides);
    tll_free(nots);
}

void
config_free(struct config *conf)
{
    free(conf->term);
    free(conf->shell);
    free(conf->title);
    free(conf->app_id);
    free(conf->word_delimiters);
    spawn_template_free(&conf->bell.command);
    free(conf->scrollback.indicator.text);
    spawn_template_free(&conf->notify);
    for (size_t i = 0; i < ALEN(conf->fonts); i++)
        config_font_list_destroy(&conf->fonts[i]);
    free(conf->server_socket_path);

    config_font_list_destroy(&conf->csd.font);

    free(conf->url.label_letters);
    spawn_template_free(&conf->url.launch);
    for (size_t i = 0; i < conf->url.prot_count; i++)
        free(conf->url.protocols[i]);
    free(conf->url.protocols);
    free(conf->url.uri_characters);

    free_key_binding_list(&conf->bindings.key);
    free_key_binding_list(&conf->bindings.search);
    free_key_binding_list(&conf->bindings.url);
    free_key_binding_list(&conf->bindings.mouse);

    tll_foreach(conf->env_vars, it) {
        free(it->item.name);
        free(it->item.value);
        tll_remove(conf->env_vars, it);
    }

    user_notifications_free(&conf->notifications);
}

bool
config_font_parse(const char *pattern, struct config_font *font)
{
    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
    if (pat == NULL)
        return false;

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

    *font = (struct config_font){
        .pattern = stripped_pattern,
        .pt_size = pt_size,
        .px_size = px_size
    };
    return true;
}

void
config_font_list_destroy(struct config_font_list *font_list)
{
    for (size_t i = 0; i < font_list->count; i++)
        free(font_list->arr[i].pattern);
    free(font_list->arr);
    font_list->count = 0;
    font_list->arr = NULL;
}


bool
check_if_font_is_monospaced(const char *pattern,
                            user_notifications_t *notifications)
{
    struct fcft_font *f = fcft_from_name(
        1, (const char *[]){pattern}, ":size=8");

    if (f == NULL)
        return true;

    static const char32_t chars[] = {U'a', U'i', U'l', U'M', U'W'};

    bool is_monospaced = true;
    int last_width = -1;

    for (size_t i = 0; i < sizeof(chars) / sizeof(chars[0]); i++) {
        const struct fcft_glyph *g = fcft_rasterize_char_utf32(
            f, chars[i], FCFT_SUBPIXEL_NONE);

        if (g == NULL)
            continue;

        if (last_width >= 0 && g->advance.x != last_width) {
            const char *font_name = f->name != NULL
                ? f->name
                : pattern;

            LOG_WARN("%s: font does not appear to be monospace; "
                     "check your config, or disable this warning by "
                     "setting [tweak].font-monospace-warn=no",
                     font_name);

            static const char fmt[] =
                "%s: font does not appear to be monospace; "
                "check your config, or disable this warning by "
                "setting \033[1m[tweak].font-monospace-warn=no\033[22m";

            user_notification_add_fmt(
                notifications, USER_NOTIFICATION_WARNING, fmt, font_name);

            is_monospaced = false;
            break;
        }

        last_width = g->advance.x;
    }

    fcft_destroy(f);
    return is_monospaced;
}

xkb_mod_mask_t
conf_modifiers_to_mask(const struct seat *seat,
                       const struct config_key_modifiers *modifiers)
{
    xkb_mod_mask_t mods = 0;
    if (seat->kbd.mod_shift != XKB_MOD_INVALID)
        mods |= modifiers->shift << seat->kbd.mod_shift;
    if (seat->kbd.mod_ctrl != XKB_MOD_INVALID)
        mods |= modifiers->ctrl << seat->kbd.mod_ctrl;
    if (seat->kbd.mod_alt != XKB_MOD_INVALID)
        mods |= modifiers->alt << seat->kbd.mod_alt;
    if (seat->kbd.mod_super != XKB_MOD_INVALID)
        mods |= modifiers->super << seat->kbd.mod_super;
    return mods;
}
