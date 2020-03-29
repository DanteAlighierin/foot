#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <assert.h>
#include <errno.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "input.h"
#include "wayland.h"

#define ALEN(v) (sizeof(v) / sizeof(v[0]))
#define min(x, y) ((x) < (y) ? (x) : (y))

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

static const char *binding_action_map[] = {
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
};

static_assert(ALEN(binding_action_map) == BIND_ACTION_COUNT,
              "binding action map size mismatch");

static char *
get_shell(void)
{
    const char *shell = getenv("SHELL");

    if (shell == NULL) {
        struct passwd *passwd = getpwuid(getuid());
        if (passwd == NULL) {
            LOG_ERRNO("failed to lookup user");
            return NULL;
        }

        shell = passwd->pw_shell;
    }

    LOG_DBG("user's shell: %s", shell);
    return strdup(shell);
}

static char *
get_config_path_user_config(void)
{
    struct passwd *passwd = getpwuid(getuid());
    if (passwd == NULL) {
        LOG_ERRNO("failed to lookup user");
        return NULL;
    }

    const char *home_dir = passwd->pw_dir;
    LOG_DBG("user's home directory: %s", home_dir);

    int len = snprintf(NULL, 0, "%s/.config/footrc", home_dir);
    char *path = malloc(len + 1);
    snprintf(path, len + 1, "%s/.config/footrc", home_dir);
    return path;
}

static char *
get_config_path_xdg(void)
{
    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    if (xdg_config_home == NULL)
        return NULL;

    int len = snprintf(NULL, 0, "%s/footrc", xdg_config_home);
    char *path = malloc(len + 1);
    snprintf(path, len + 1, "%s/footrc", xdg_config_home);
    return path;
}

static char *
get_config_path(void)
{
    struct stat st;

    char *config = get_config_path_xdg();
    if (config != NULL && stat(config, &st) == 0 && S_ISREG(st.st_mode))
        return config;
    free(config);

    /* 'Default' XDG_CONFIG_HOME */
    config = get_config_path_user_config();
    if (config != NULL && stat(config, &st) == 0 && S_ISREG(st.st_mode))
        return config;
    free(config);

    return NULL;
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
str_to_color(const char *s, uint32_t *color, bool allow_alpha, const char *path, int lineno)
{
    unsigned long value;
    if (!str_to_ulong(s, 16, &value)) {
        LOG_ERRNO("%s:%d: invalid color: %s", path, lineno, s);
        return false;
    }

    if (!allow_alpha && (value & 0xff000000) != 0) {
        LOG_ERR("%s:%d: color value must not have an alpha component", path, lineno);
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
        conf->term = strdup(value);
    }

    else if (strcmp(key, "shell") == 0) {
        free(conf->shell);
        conf->shell = strdup(value);
    }

    else if (strcmp(key, "login-shell") == 0) {
        conf->login_shell = str_to_bool(value);
    }

    else if (strcmp(key, "geometry") == 0) {
        unsigned width, height;
        if (sscanf(value, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
            LOG_ERR(
                "%s: %d: expected WIDTHxHEIGHT, where both are positive integers: %s",
                path, lineno, value);
            return false;
        }

        conf->width = width;
        conf->height = height;
    }

    else if (strcmp(key, "pad") == 0) {
        unsigned x, y;
        if (sscanf(value, "%ux%u", &x, &y) != 2) {
            LOG_ERR(
                "%s:%d: expected PAD_XxPAD_Y, where both are positive integers: %s",
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
            LOG_ERR(
                "%s:%d: expected either 'windowed', 'maximized' or 'fullscreen'",
                path, lineno);
            return false;
        }
    }

    else if (strcmp(key, "font") == 0) {
        char *copy = strdup(value);
        for (const char *font = strtok(copy, ","); font != NULL; font = strtok(NULL, ",")) {
            /* Trim spaces, strictly speaking not necessary, but looks nice :) */
            while (*font != '\0' && isspace(*font))
                font++;
            if (*font != '\0')
                tll_push_back(conf->fonts, strdup(font));
        }
        free(copy);
    }

    else if (strcmp(key, "workers") == 0) {
        unsigned long count;
        if (!str_to_ulong(value, 10, &count)) {
            LOG_ERR("%s:%d: expected an integer: %s", path, lineno, value);
            return false;
        }
        conf->render_worker_count = count;
    }

    else if (strcmp(key, "scrollback") == 0) {
        unsigned long lines;
        if (!str_to_ulong(value, 10, &lines)) {
            LOG_ERR("%s:%d: expected an integer: %s", path, lineno, value);
            return false;
        }
        conf->scrollback_lines = lines;
    }

    else {
        LOG_ERR("%s:%u: invalid key: %s", path, lineno, key);
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
    else if (strcmp(key, "alpha") == 0) {
        double alpha;
        if (!str_to_double(value, &alpha) || alpha < 0. || alpha > 1.) {
            LOG_ERR("%s: %d: alpha: expected a value in the range 0.0-1.0",
                    path, lineno);
            return false;
        }

        conf->colors.alpha = alpha * 65535.;
        return true;
    }

    else {
        LOG_ERR("%s:%d: invalid key: %s", path, lineno, key);
        return false;
    }

    uint32_t color_value;
    if (!str_to_color(value, &color_value, false, path, lineno))
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
            LOG_ERR("%s:%d: invalid 'style': %s", path, lineno, value);
            return false;
        }
    }

    else if (strcmp(key, "color") == 0) {
        char *value_copy = strdup(value);
        const char *text = strtok(value_copy, " ");
        const char *cursor = strtok(NULL, " ");

        uint32_t text_color, cursor_color;
        if (text == NULL || cursor == NULL ||
            !str_to_color(text, &text_color, false, path, lineno) ||
            !str_to_color(cursor, &cursor_color, false, path, lineno))
        {
            LOG_ERR("%s:%d: invalid cursor colors: %s", path, lineno, value);
            free(value_copy);
            return false;
        }

        conf->cursor.color.text = 1 << 31 | text_color;
        conf->cursor.color.cursor = 1 << 31 | cursor_color;
        free(value_copy);
    }

    else {
        LOG_ERR("%s:%d: invalid key: %s", path, lineno, key);
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
            LOG_ERR("%s:%d: expected either 'server' or 'client'", path, lineno);
            return false;
        }
    }

    else if (strcmp(key, "color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, path, lineno)) {
            LOG_ERR("%s:%d: invalid titlebar-color: %s", path, lineno, value);
            return false;
        }

        conf->csd.color.title_set = true;
        conf->csd.color.title = color;
    }

    else if (strcmp(key, "size") == 0) {
        unsigned long pixels;
        if (!str_to_ulong(value, 10, &pixels)) {
            LOG_ERR("%s:%d: expected an integer: %s", path, lineno, value);
            return false;
        }

        conf->csd.title_height = pixels;
    }

    else if (strcmp(key, "button-width") == 0) {
        unsigned long pixels;
        if (!str_to_ulong(value, 10, &pixels)) {
            LOG_ERR("%s:%d: expected an integer: %s", path, lineno, value);
            return false;
        }

        conf->csd.button_width = pixels;
    }

    else if (strcmp(key, "button-minimize-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, path, lineno)) {
            LOG_ERR("%s:%d: invalid button-minimize-color: %s", path, lineno, value);
            return false;
        }

        conf->csd.color.minimize_set = true;
        conf->csd.color.minimize = color;
    }

    else if (strcmp(key, "button-maximize-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, path, lineno)) {
            LOG_ERR("%s:%d: invalid button-maximize-color: %s", path, lineno, value);
            return false;
        }

        conf->csd.color.maximize_set = true;
        conf->csd.color.maximize = color;
    }

    else if (strcmp(key, "button-close-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, path, lineno)) {
            LOG_ERR("%s:%d: invalid button-close-color: %s", path, lineno, value);
            return false;
        }

        conf->csd.color.close_set = true;
        conf->csd.color.close = color;
    }

    else {
        LOG_ERR("%s:%u: invalid key: %s", path, lineno, key);
        return false;
    }

    return true;
}

static bool
verify_key_combo(const struct config *conf, const char *combo, const char *path,
                 unsigned lineno)
{
    for (enum bind_action_normal action = 0; action < BIND_ACTION_COUNT; action++) {
        if (conf->bindings.key[action] == NULL)
            continue;

        char *copy = strdup(conf->bindings.key[action]);

        for (char *save = NULL, *collision = strtok_r(copy, " ", &save);
             collision != NULL;
             collision = strtok_r(NULL, " ", &save))
        {
            if (strcmp(combo, collision) == 0) {
                LOG_ERR("%s:%d: %s already mapped to %s", path, lineno, combo, binding_action_map[action]);
                free(copy);
                return false;
            }
        }

        free(copy);
    }

    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(
        ctx, &(struct xkb_rule_names){0}, XKB_KEYMAP_COMPILE_NO_FLAGS);

    bool valid_combo = input_parse_key_binding(keymap, combo, NULL);

    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);

    if (!valid_combo) {
        LOG_ERR("%s:%d: invalid key combination: %s", path, lineno, combo);
        return false;
    }

    return true;
}

static bool
parse_section_key_bindings(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno)
{
    for (enum bind_action_normal action = 0; action < BIND_ACTION_COUNT; action++) {
        if (binding_action_map[action] == NULL)
            continue;

        if (strcmp(key, binding_action_map[action]) != 0)
            continue;

        if (strcasecmp(value, "none") == 0) {
            free(conf->bindings.key[action]);
            conf->bindings.key[action] = NULL;
            return true;
        }

        if (!verify_key_combo(conf, value, path, lineno)) {
            return false;
        }

        free(conf->bindings.key[action]);
        conf->bindings.key[action] = strdup(value);
        return true;
    }

    LOG_ERR("%s:%u: invalid key: %s", path, lineno, key);
    return false;

}

static bool
parse_section_mouse_bindings(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno)
{
    for (enum bind_action_normal action = 0; action < BIND_ACTION_COUNT; action++) {
        if (binding_action_map[action] == NULL)
            continue;

        if (strcmp(key, binding_action_map[action]) != 0)
            continue;

        if (strcmp(value, "NONE") == 0) {
            conf->bindings.mouse[action] = (struct mouse_binding){0, 0, BIND_ACTION_NONE};
            return true;
        }

        const char *map[] = {
            [BTN_LEFT] = "BTN_LEFT",
            [BTN_RIGHT] = "BTN_RIGHT",
            [BTN_MIDDLE] = "BTN_MIDDLE",
            [BTN_SIDE] = "BTN_SIDE",
            [BTN_EXTRA] = "BTN_EXTRA",
            [BTN_FORWARD] = "BTN_FORWARD",
            [BTN_BACK] = "BTN_BACK",
            [BTN_TASK] = "BTN_TASK",
        };

        for (size_t i = 0; i < ALEN(map); i++) {
            if (map[i] == NULL || strcmp(map[i], value) != 0)
                continue;

            const int count = 1;

            /* Make sure button isn't already mapped to another action */
            for (enum bind_action_normal j = 0; j < BIND_ACTION_COUNT; j++) {
                const struct mouse_binding *collision = &conf->bindings.mouse[j];
                if (collision->button == i && collision->count == count) {
                    LOG_ERR("%s:%d: %s already mapped to %s", path, lineno,
                            value, binding_action_map[collision->action]);
                    return false;
                }
            }

            conf->bindings.mouse[action] = (struct mouse_binding){i, count, action};
            return true;
        }

        LOG_ERR("%s:%d: invalid mouse button: %s", path, lineno, value);
        return false;

    }

    LOG_ERR("%s:%u: invalid key: %s", path, lineno, key);
    return false;
}

static bool
parse_section_tweak(
    const char *key, const char *value, struct config *conf,
    const char *path, unsigned lineno)
{
    if (strcmp(key, "delayed-render-lower") == 0) {
        unsigned long ns;
        if (!str_to_ulong(value, 10, &ns)) {
            LOG_ERR("%s:%d: expected an integer: %s", path, lineno, value);
            return false;
        }

        if (ns > 16666666) {
            LOG_ERR("%s:%d: timeout must not exceed 16ms", path, lineno);
            return false;
        }

        conf->tweak.delayed_render_lower_ns = ns;
        LOG_WARN("tweak: delayed-render-lower=%lu", ns);
    }

    else if (strcmp(key, "delayed-render-upper") == 0) {
        unsigned long ns;
        if (!str_to_ulong(value, 10, &ns)) {
            LOG_ERR("%s:%d: expected an integer: %s", path, lineno, value);
            return false;
        }

        if (ns > 16666666) {
            LOG_ERR("%s:%d: timeout must not exceed 16ms", path, lineno);
            return false;
        }

        conf->tweak.delayed_render_upper_ns = ns;
        LOG_WARN("tweak: delayed-render-upper=%lu", ns);
    }

    else if (strcmp(key, "max-shm-pool-size-mb") == 0) {
        unsigned long mb;
        if (!str_to_ulong(value, 10, &mb)) {
            LOG_ERR("%s:%d: expected an integer: %s", path, lineno, value);
            return false;
        }

        conf->tweak.max_shm_pool_size = min(mb * 1024 * 1024, INT32_MAX);
        LOG_WARN("tweak: max-shm-pool-size=%lu bytes",
                 conf->tweak.max_shm_pool_size);
    }

    else {
        LOG_ERR("%s:%u: invalid key: %s", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_config_file(FILE *f, struct config *conf, const char *path)
{
    enum section {
        SECTION_MAIN,
        SECTION_COLORS,
        SECTION_CURSOR,
        SECTION_CSD,
        SECTION_KEY_BINDINGS,
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
        [SECTION_MAIN] =           {&parse_section_main, "main"},
        [SECTION_COLORS] =         {&parse_section_colors, "colors"},
        [SECTION_CURSOR] =         {&parse_section_cursor, "cursor"},
        [SECTION_CSD] =            {&parse_section_csd, "csd"},
        [SECTION_KEY_BINDINGS] =   {&parse_section_key_bindings, "key-bindings"},
        [SECTION_MOUSE_BINDINGS] = {&parse_section_mouse_bindings, "mouse-bindings"},
        [SECTION_TWEAK] =          {&parse_section_tweak, "tweak"},
    };

    static_assert(ALEN(section_info) == SECTION_COUNT, "section info array size mismatch");

    unsigned lineno = 0;

    char *_line = NULL;
    size_t count = 0;

    while (true) {
        errno = 0;
        lineno++;

        ssize_t ret = getline(&_line, &count, f);

        if (ret < 0) {
            if (errno != 0) {
                LOG_ERRNO("failed to read from configuration");
                goto err;
            }
            break;
        }

        /* Strip whitespace */
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

        /* Check for new section */
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end == NULL) {
                LOG_ERR("%s:%d: syntax error: %s", path, lineno, line);
                goto err;
            }

            *end = '\0';

            section = SECTION_COUNT;
            for (enum section i = 0; i < SECTION_COUNT; i++) {
                if (strcmp(&line[1], section_info[i].name) == 0) {
                    section = i;
                }
            }

            if (section == SECTION_COUNT) {
                LOG_ERR("%s:%d: invalid section name: %s", path, lineno, &line[1]);
                goto err;
            }

            /* Process next line */
            continue;
        }

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "\n");

        /* Strip trailing whitespace from key (leading stripped earlier) */
        {
            assert(!isspace(*key));

            char *end = key + strlen(key) - 1;
            while (isspace(*end))
                end--;
            *(end + 1) = '\0';
        }

        if (value == NULL) {
            if (key != NULL && strlen(key) > 0 && key[0] != '#') {
                LOG_ERR("%s:%d: syntax error: %s", path, lineno, line);
                goto err;
            }

            continue;
        }

        /* Strip leading whitespace from value (trailing stripped earlier) */
        {
            while (isspace(*value))
                value++;
            assert(!isspace(*(value + strlen(value) - 1)));
        }

        if (key[0] == '#')
            continue;

        LOG_DBG("section=%s, key='%s', value='%s'",
                section_names[section], key, value);

        parser_fun_t section_parser = section_info[section].fun;
        assert(section_parser != NULL);

        if (!section_parser(key, value, conf, path, lineno))
            goto err;
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
    const char *xdg_session_id = getenv("XDG_SESSION_ID");
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime == NULL)
        return strdup("/tmp/foot.sock");

    if (xdg_session_id == NULL)
        xdg_session_id = "<no-session>";

    char *path = malloc(strlen(xdg_runtime) + 1 + strlen("foot-.sock") + strlen(xdg_session_id) + 1);
    sprintf(path, "%s/foot-%s.sock", xdg_runtime, xdg_session_id);
    return path;
}

bool
config_load(struct config *conf, const char *conf_path)
{
    bool ret = false;

    *conf = (struct config) {
        .term = strdup("foot"),
        .shell = get_shell(),
        .width = 700,
        .height = 500,
        .pad_x = 2,
        .pad_y = 2,
        .startup_mode = STARTUP_WINDOWED,
        .fonts = tll_init(),
        .scrollback_lines = 1000,

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
        },

        .cursor = {
            .style = CURSOR_BLOCK,
            .color = {
                .text = 0,
                .cursor = 0,
            },
        },

        .bindings = {
            .key = {
                [BIND_ACTION_SCROLLBACK_UP] = strdup("Shift+Page_Up"),
                [BIND_ACTION_SCROLLBACK_DOWN] = strdup("Shift+Page_Down"),
                [BIND_ACTION_CLIPBOARD_COPY] = strdup("Control+Shift+C"),
                [BIND_ACTION_CLIPBOARD_PASTE] = strdup("Control+Shift+V"),
                [BIND_ACTION_SEARCH_START] = strdup("Control+Shift+R"),
                [BIND_ACTION_FONT_SIZE_UP] = strdup("Control+plus Control+equal Control+KP_Add"),
                [BIND_ACTION_FONT_SIZE_DOWN] = strdup("Control+minus Control+KP_Subtract"),
                [BIND_ACTION_FONT_SIZE_RESET] = strdup("Control+0 Control+KP_0"),
                [BIND_ACTION_SPAWN_TERMINAL] = strdup("Control+Shift+N"),
            },
            .mouse = {
                [BIND_ACTION_PRIMARY_PASTE] = {BTN_MIDDLE, 1, BIND_ACTION_PRIMARY_PASTE},
            },
            .search = {
                [BIND_ACTION_SEARCH_CANCEL] = strdup("Control+g Escape"),
                [BIND_ACTION_SEARCH_COMMIT] = strdup("Return"),
                [BIND_ACTION_SEARCH_FIND_PREV] = strdup("Control+r"),
                [BIND_ACTION_SEARCH_FIND_NEXT] = strdup("Control+s"),
                [BIND_ACTION_SEARCH_EDIT_LEFT] = strdup("Left Control+b"),
                [BIND_ACTION_SEARCH_EDIT_LEFT_WORD] = strdup("Control+Left Mod1+b"),
                [BIND_ACTION_SEARCH_EDIT_RIGHT] = strdup("Right Control+f"),
                [BIND_ACTION_SEARCH_EDIT_RIGHT_WORD] = strdup("Control+Right Mod1+f"),
                [BIND_ACTION_SEARCH_EDIT_HOME] = strdup("Home Control+a"),
                [BIND_ACTION_SEARCH_EDIT_END] = strdup("End Control+e"),
                [BIND_ACTION_SEARCH_DELETE_PREV] = strdup("BackSpace"),
                [BIND_ACTION_SEARCH_DELETE_PREV_WORD] = strdup("Mod1+BackSpace Control+BackSpace"),
                [BIND_ACTION_SEARCH_DELETE_NEXT] = strdup("Delete "),
                [BIND_ACTION_SEARCH_DELETE_NEXT_WORD] = strdup("Mod1+d Control+Delete"),
                [BIND_ACTION_SEARCH_EXTEND_WORD] = strdup("Control+w"),
                [BIND_ACTION_SEARCH_EXTEND_WORD_WS] = strdup("Control+Shift+W"),
            },
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
        },
    };

    char *default_path = NULL;
    if (conf_path == NULL) {
        if ((default_path = get_config_path()) == NULL) {
            /* Default conf */
            LOG_WARN("no configuration found, using defaults");
            ret = true;
            goto out;
        }

        conf_path = default_path;
    }

    assert(conf_path != NULL);
    LOG_INFO("loading configuration from %s", conf_path);

    FILE *f = fopen(conf_path, "r");
    if (f == NULL) {
        LOG_ERR("%s: failed to open", conf_path);
        goto out;
    }

    ret = parse_config_file(f, conf, conf_path);
    fclose(f);

out:
    if (ret && tll_length(conf->fonts) == 0)
        tll_push_back(conf->fonts, strdup("monospace"));

    free(default_path);
    return ret;
}

void
config_free(struct config conf)
{
    free(conf.term);
    free(conf.shell);
    tll_free_and_free(conf.fonts, free);
    free(conf.server_socket_path);

    for (enum bind_action_normal i = 0; i < BIND_ACTION_COUNT; i++)
        free(conf.bindings.key[i]);
    for (enum bind_action_search i = 0; i < BIND_ACTION_SEARCH_COUNT; i++)
        free(conf.bindings.search[i]);
}
