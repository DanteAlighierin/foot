#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <assert.h>
#include <errno.h>

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 0
#include "log.h"

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
parse_section_main(char *line, struct config *conf, const char *path, unsigned lineno)
{
    const char *key = strtok(line, "=");
    const char *value = strtok(NULL, "\n");

    LOG_DBG("%s:%u: key = %s, value=%s", path, lineno, key, value);

    if (strcmp(key, "font") == 0) {
        free(conf->font);
        conf->font = strdup(value);
    }

    else if (strcmp(key, "shell") == 0) {
        free(conf->shell);
        conf->shell = strdup(value);
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
    } section = SECTION_MAIN;

    /* Function pointer, called for each key/value line */
    typedef bool (*parser_fun_t)(
        char *line, struct config *conf, const char *path, unsigned lineno);

    /* Maps sections to line parser functions */
    static const parser_fun_t section_parser_map[] = {
        [SECTION_MAIN] = &parse_section_main,
    };

    unsigned lineno = 0;

    while (true) {
        errno = 0;
        lineno++;

        char *line = NULL;
        size_t count = 0;
        ssize_t ret = getline(&line, &count, f);

        if (ret < 0) {
            free(line);
            if (errno != 0) {
                LOG_ERRNO("failed to read from configuration");
                return false;
            }
            break;
        }

        /* No sections yet */
        if (line[0] == '[' && line[strlen(line) - 1] == ']') {
            assert(false);
            return false;
        }

        parser_fun_t section_parser = section_parser_map[section];
        assert(section_parser != NULL);

        if (!section_parser(line, conf, path, lineno)) {
            free(line);
            return false;
        }

        free(line);
    }

    return true;
}

struct config
config_load(void)
{
    struct config conf = {
        .shell = get_shell(),
        .font = strdup("monospace"),
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

    parse_config_file(f, &conf, path);
    fclose(f);

out:
    free(path);
    return conf;
}

void
config_free(struct config conf)
{
    free(conf.shell);
    free(conf.font);
}
