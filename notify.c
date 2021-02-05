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

    if (term->kbd_focus) {
        /* No notifications while we’re focused */
        return;
    }

    if (title == NULL || body == NULL)
        return;

    if (term->conf->notify.argv == NULL)
        return;

    size_t argv_size = 0;
    for (; term->conf->notify.argv[argv_size] != NULL; argv_size++)
        ;

#define append(s, n)                                        \
    do {                                                    \
        expanded = xrealloc(expanded, len + (n) + 1);       \
        memcpy(&expanded[len], s, n);                       \
        len += n;                                           \
        expanded[len] = '\0';                               \
    } while (0)

    char **argv = malloc((argv_size + 1) * sizeof(argv[0]));

    /* Expand ${title} and ${body} */
    for (size_t i = 0; i < argv_size; i++) {
        size_t len = 0;
        char *expanded = NULL;

        char *start = NULL;
        char *last_end = term->conf->notify.argv[i];

        while ((start = strstr(last_end, "${")) != NULL) {
            /* Append everything from the last template's end to this
             * one's beginning */
            append(last_end, start - last_end);

            /* Find end of template */
            start += 2;
            char *end = strstr(start, "}");

            if (end == NULL) {
                /* Ensure final append() copies the unclosed '${' */
                last_end = start - 2;
                LOG_WARN("notify: unclosed template: %s", last_end);
                break;
            }

            /* Expand template */
            if (strncmp(start, "title", end - start) == 0)
                append(title, strlen(title));
            else if (strncmp(start, "body", end - start) == 0)
                append(body, strlen(body));
            else {
                /* Unrecognized template - append it as-is */
                start -= 2;
                append(start, end + 1 - start);
                LOG_WARN("notify: unrecognized template: %.*s",
                         (int)(end + 1 - start), start);
            }

            last_end = end + 1;;
        }

        append(last_end, term->conf->notify.argv[i] + strlen(term->conf->notify.argv[i]) - last_end);
        argv[i] = expanded;
    }
    argv[argv_size] = NULL;

#undef append

    LOG_DBG("notify command:");
    for (size_t i = 0; i < argv_size; i++)
        LOG_DBG("  argv[%zu] = \"%s\"", i, argv[i]);

    /* Redirect stdin to /dev/null, but ignore failure to open */
    int devnull = open("/dev/null", O_RDONLY);
    spawn(term->reaper, NULL, argv, devnull, -1, -1);

    if (devnull >= 0)
        close(devnull);

    for (size_t i = 0; i < argv_size; i++)
        free(argv[i]);
    free(argv);
}
