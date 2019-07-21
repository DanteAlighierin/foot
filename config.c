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
    0x000000,
    0xcc9393,
    0x7f9f7f,
    0xd0bf8f,
    0x6ca0a3,
    0xdc8cc3,
    0x93e0e3,
    0xdcdccc,
};

static const uint32_t default_bright[] = {
    0x000000,
    0xdca3a3,
    0xbfebbf,
    0xf0dfaf,
    0x8cd0d3,
    0xdc8cc3,
    0x93e0e3,
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

    else if (strcmp(key, "font") == 0) {
        free(conf->font);
        conf->font = strdup(value);
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

    else {
        LOG_ERR("%s:%d: invalid key: %s", path, lineno, key);
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long res = strtoul(value, &end, 16);

    if (errno != 0) {
        LOG_ERRNO("%s:%d: invalid color: %s", path, lineno, value);
        return false;
    }

    if (*end != '\0') {
        LOG_ERR("%s:%d: invalid color: %s", path, lineno, value);
        return false;
    }

    *color = res & 0xffffff;
    return true;
}

static bool
parse_config_file(FILE *f, struct config *conf, const char *path)
{
    enum section {
        SECTION_MAIN,
        SECTION_COLORS,
    } section = SECTION_MAIN;

    /* Function pointer, called for each key/value line */
    typedef bool (*parser_fun_t)(
        const char *key, const char *value, struct config *conf,
        const char *path, unsigned lineno);

    /* Maps sections to line parser functions */
    static const parser_fun_t section_parser_map[] = {
        [SECTION_MAIN] = &parse_section_main,
        [SECTION_COLORS] = &parse_section_colors,
    };

#if defined(_DEBUG) && defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    static const char *const section_names[] = {
        [SECTION_MAIN] = "main",
        [SECTION_COLORS] = "colors",
    };
#endif

    unsigned lineno = 0;
    char *_line = NULL;

    while (true) {
        errno = 0;
        lineno++;

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

bool
config_load(struct config *conf)
{
    bool ret = false;

    *conf = (struct config) {
        .term = strdup("foot"),
        .shell = get_shell(),
        .font = strdup("monospace"),

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
        },
    };

    char *path = get_config_path();
    LOG_INFO("loading configuration from %s", path);

    if (path == NULL) {
        /* Default conf */
        LOG_WARN("no configuration found, using defaults");
        goto out;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        LOG_ERR("%s: failed to open", path);
        goto out;
    }

    ret = parse_config_file(f, conf, path);
    fclose(f);

out:
    free(path);
    return ret;
}

void
config_free(struct config conf)
{
    free(conf.term);
    free(conf.shell);
    free(conf.font);
}
