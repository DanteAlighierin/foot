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

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 0
#include "log.h"

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

static char *
get_shell(void)
{
    struct passwd *passwd = getpwuid(getuid());
    if (passwd == NULL) {
        LOG_ERRNO("failed to lookup user");
        return NULL;
    }

    const char *shell = passwd->pw_shell;
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
        conf->login_shell = (
            strcasecmp(value, "on") == 0 ||
            strcasecmp(value, "true") == 0 ||
            strcasecmp(value, "yes") == 0) ||
            strtoul(value, NULL, 0) > 0;
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
                "%s: %d: expected PAD_XxPAD_Y, where both are positive integers: %s",
                path, lineno, value);
            return false;
        }

        conf->pad_x = x;
        conf->pad_y = y;
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
        LOG_WARN("%s:%u: invalid key: %s", path, lineno, key);
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

    else if (strcmp(key, "titlebar-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, path, lineno)) {
            LOG_ERR("%s:%d: invalid titlebar-color: %s", path, lineno, value);
            return false;
        }

        conf->csd.color.title_set = true;
        conf->csd.color.title = color;
    }

    else if (strcmp(key, "border-color") == 0) {
        uint32_t color;
        if (!str_to_color(value, &color, true, path, lineno)) {
            LOG_ERR("%s:%d: invalid border-color: %s", path, lineno, value);
            return false;
        }

        conf->csd.color.border_set = true;
        conf->csd.color.border = color;
    }

    else if (strcmp(key, "titlebar") == 0) {
        unsigned long pixels;
        if (!str_to_ulong(value, 10, &pixels)) {
            LOG_ERR("%s:%d: expected an integer: %s", path, lineno, value);
            return false;
        }

        conf->csd.title_height = pixels;
    }

    else if (strcmp(key, "border") == 0) {
        unsigned long pixels;
        if (!str_to_ulong(value, 10, &pixels)) {
            LOG_ERR("%s:%d: expected an integer: %s", path, lineno, value);
            return false;
        }

        conf->csd.border_width = pixels;
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
    } section = SECTION_MAIN;

    /* Function pointer, called for each key/value line */
    typedef bool (*parser_fun_t)(
        const char *key, const char *value, struct config *conf,
        const char *path, unsigned lineno);

    /* Maps sections to line parser functions */
    static const parser_fun_t section_parser_map[] = {
        [SECTION_MAIN] = &parse_section_main,
        [SECTION_COLORS] = &parse_section_colors,
        [SECTION_CURSOR] = &parse_section_cursor,
        [SECTION_CSD] = &parse_section_csd,
    };

#if defined(_DEBUG) && defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    static const char *const section_names[] = {
        [SECTION_MAIN] = "main",
        [SECTION_COLORS] = "colors",
        [SECTION_CURSOR] = "cursor",
        [SECTION_CSD] = "csd",
    };
#endif

    unsigned lineno = 0;
    char *_line = NULL;

    while (true) {
        errno = 0;
        lineno++;

        _line = NULL;
        size_t count = 0;
        ssize_t ret = getline(&_line, &count, f);

        if (ret < 0) {
            free(_line);
            if (errno != 0) {
                LOG_ERRNO("failed to read from configuration");
                return false;
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
        if (line[0] == '\0' || line[0] == '#') {
            free(_line);
            continue;
        }

        /* Check for new section */
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end == NULL) {
                LOG_ERR("%s:%d: syntax error: %s", path, lineno, line);
                goto err;
            }

            *end = '\0';

            if (strcmp(&line[1], "colors") == 0)
                section = SECTION_COLORS;
            else if (strcmp(&line[1], "cursor") == 0)
                section = SECTION_CURSOR;
            else if (strcmp(&line[1], "csd") == 0)
                section = SECTION_CSD;
            else {
                LOG_ERR("%s:%d: invalid section name: %s", path, lineno, &line[1]);
                goto err;
            }

            free(_line);
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

            free(_line);
            continue;
        }

        /* Strip leading whitespace from value (trailing stripped earlier) */
        {
            while (isspace(*value))
                value++;
            assert(!isspace(*(value + strlen(value) - 1)));
        }

        if (key[0] == '#') {
            free(_line);
            continue;
        }

        LOG_DBG("section=%s, key='%s', value='%s'",
                section_names[section], key, value);

        parser_fun_t section_parser = section_parser_map[section];
        assert(section_parser != NULL);

        if (!section_parser(key, value, conf, path, lineno))
            goto err;

        free(_line);
    }

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
        return strdup("/tmp/foot.sock");

    char *path = malloc(strlen(xdg_runtime) + 1 + strlen("foot.sock") + 1);
    sprintf(path, "%s/foot.sock", xdg_runtime);
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

        .csd = {
            .preferred = CONF_CSD_PREFER_SERVER,
            .title_height = 26,
            .border_width = 5,
            .color = {
                .title_set = false,
                .border_set = false,
            },
        },

        .render_worker_count = sysconf(_SC_NPROCESSORS_ONLN),
        .server_socket_path = get_server_socket_path(),
        .presentation_timings = false,
        .hold_at_exit = false,
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
}
