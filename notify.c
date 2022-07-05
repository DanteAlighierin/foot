#include "notify.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "notify"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "spawn.h"
#include "xmalloc.h"

void
notify_notify(const struct terminal *term, const char *title, const char *body)
{
    LOG_DBG("notify: title=\"%s\", msg=\"%s\"", title, body);

    if (term->conf->notify_focus_inhibit && term->kbd_focus) {
        /* No notifications while we’re focused */
        return;
    }

    if (title == NULL || body == NULL)
        return;

    if (term->conf->notify.argv.args == NULL)
        return;

    char **argv = NULL;
    size_t argc = 0;

    if (!spawn_expand_template(
            &term->conf->notify, 4,
            (const char *[]){"app-id", "window-title", "title", "body"},
            (const char *[]){term->conf->app_id, term->window_title, title, body},
            &argc, &argv))
    {
        return;
    }

    LOG_DBG("notify command:");
    for (size_t i = 0; i < argc; i++)
        LOG_DBG("  argv[%zu] = \"%s\"", i, argv[i]);

    /* Redirect stdin to /dev/null, but ignore failure to open */
    int devnull = open("/dev/null", O_RDONLY);
    spawn(term->reaper, NULL, argv, devnull, -1, -1, NULL);

    if (devnull >= 0)
        close(devnull);

    for (size_t i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
}
