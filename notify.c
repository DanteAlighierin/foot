#include "notify.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "notify"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "config.h"
#include "spawn.h"
#include "terminal.h"
#include "util.h"
#include "wayland.h"
#include "xmalloc.h"
#include "xsnprintf.h"

void
notify_free(struct terminal *term, struct notification *notif)
{
    if (notif->pid > 0)
        fdm_del(term->fdm, notif->stdout_fd);

    free(notif->id);
    free(notif->title);
    free(notif->body);
    free(notif->category);
    free(notif->app_id);
    free(notif->icon_cache_id);
    free(notif->icon_symbolic_name);
    free(notif->icon_data);
    free(notif->sound_name);
    free(notif->xdg_token);
    free(notif->stdout_data);

    tll_free_and_free(notif->actions, free);

    if (notif->icon_path != NULL) {
        unlink(notif->icon_path);
        free(notif->icon_path);

        if (notif->icon_fd >= 0)
            close(notif->icon_fd);
    }

    memset(notif, 0, sizeof(*notif));
}

static bool
write_icon_file(const void *data, size_t data_sz, int *fd, char **filename,
                char **symbolic_name)
{
    xassert(*filename == NULL);
    xassert(*symbolic_name == NULL);

    char name[64] = "/tmp/foot-notification-icon-XXXXXX";

    *filename = NULL;
    *symbolic_name = NULL;
    *fd = mkostemp(name, O_CLOEXEC);

    if (*fd < 0) {
        LOG_ERRNO("failed to create temporary file for icon cache");
        return false;
    }

    if (write(*fd, data, data_sz) != (ssize_t)data_sz) {
        LOG_ERRNO("failed to write icon data to temporary file");
        close(*fd);
        *fd = -1;
        return false;
    }

    LOG_DBG("wrote icon data to %s", name);
    *filename = xstrdup(name);
    *symbolic_name = xstrjoin("file://", *filename);
    return true;
}

static bool
to_integer(const char *line, size_t len, uint32_t *res)
{
    bool is_id = true;
    uint32_t maybe_id = 0;

    for (size_t i = 0; i < len; i++) {
        char digit = line[i];
        if (digit < '0' || digit > '9') {
            is_id = false;
            break;
        }

        maybe_id *= 10;
        maybe_id += digit - '0';
    }

    *res = maybe_id;
    return is_id;
}

static void
consume_stdout(struct notification *notif, bool eof)
{
    char *data = notif->stdout_data;
    const char *line = data;
    size_t left = notif->stdout_sz;

    /* Process stdout, line-by-line */
    while (left > 0) {
        line = data;
        size_t len = left;
        char *eol = memchr(line, '\n', left);

        if (eol != NULL) {
            *eol = '\0';
            len = strlen(line);
            data = eol + 1;
        } else if (!eof)
            break;

        uint32_t maybe_id = 0;
        uint32_t maybe_button_nr = 0;

        /* Check for daemon assigned ID, either '123', or 'id=123' */
        if ((notif->external_id == 0 && to_integer(line, len, &maybe_id)) ||
            (len > 3 && memcmp(line, "id=", 3) == 0 &&
             to_integer(&line[3], len - 3, &maybe_id)))
        {
            notif->external_id = maybe_id;
            LOG_DBG("external ID: %u", notif->external_id);
        }

        /* Check for triggered action, either 'default' or 'action=default' */
        else if ((len == 7 && memcmp(line, "default", 7) == 0) ||
                 (len == 7 + 7 && memcmp(line, "action=default", 7 + 7) == 0))
        {
            notif->activated = true;
            LOG_DBG("notification's default action was triggered");
        }

        else if (len > 7 && memcmp(line, "action=", 7) == 0) {
            notif->activated = true;

            if (to_integer(&line[7], len - 7, &maybe_button_nr)) {
                notif->activated_button = maybe_button_nr;
                LOG_DBG("custom action %u triggered", notif->activated_button);
            } else {
                LOG_DBG("unrecognized action triggered: %.*s",
                        (int)(len - 7), &line[7]);
            }
        }

        else if (notif->external_id > 0 &&
                 to_integer(line, len, &maybe_button_nr) &&
                 maybe_button_nr > 0 &&
                 maybe_button_nr <= notif->button_count)
        {
            /* Single integer, appearing *after* the ID, and is within
               the custom button/action range */
            notif->activated = true;
            notif->activated_button = maybe_button_nr;
            LOG_DBG("custom action %u triggered", notif->activated_button);
        }

        /* Check for XDG activation token, 'xdgtoken=xyz' */
        else if (len > 9 && memcmp(line, "xdgtoken=", 9) == 0) {
            notif->xdg_token = xstrndup(&line[9], len - 9);
            LOG_DBG("XDG token: \"%s\"", notif->xdg_token);
        }

        left -= len + (eol != NULL ? 1 : 0);
    }

    if (left > 0)
        memmove(notif->stdout_data, data, left);

    notif->stdout_sz = left;
}

static bool
fdm_notify_stdout(struct fdm *fdm, int fd, int events, void *data)
{
    const struct terminal *term = data;
    struct notification *notif = NULL;

    /* Find notification */
    tll_foreach(term->active_notifications, it) {
        if (it->item.stdout_fd == fd) {
            notif = &it->item;
            break;
        }
    }

    if (events & EPOLLIN) {
        char buf[512];
        ssize_t count = read(fd, buf, sizeof(buf) - 1);

        if (count < 0) {
            if (errno == EINTR)
                return true;

            LOG_ERRNO("failed to read notification activation token");
            return false;
        }

        if (count > 0 && notif != NULL) {
            if (notif->stdout_data == NULL) {
                xassert(notif->stdout_sz == 0);
                notif->stdout_data = xmemdup(buf, count);
            } else {
                notif->stdout_data = xrealloc(notif->stdout_data, notif->stdout_sz + count);
                memcpy(&notif->stdout_data[notif->stdout_sz], buf, count);
            }

            notif->stdout_sz += count;
            consume_stdout(notif, false);
        }
    }

    if (events & EPOLLHUP) {
        fdm_del(fdm, fd);
        if (notif != NULL) {
            notif->stdout_fd = -1;
            consume_stdout(notif, true);
        }
    }

    return true;
}

static void
notif_done(struct reaper *reaper, pid_t pid, int status, void *data)
{
    struct terminal *term = data;

    tll_foreach(term->active_notifications, it) {
        struct notification *notif = &it->item;
        if (notif->pid != pid)
            continue;

        LOG_DBG("notification %s closed",
                notif->id != NULL ? notif->id : "<unset>");

        if (notif->activated && notif->focus) {
            LOG_DBG("focus window on notification activation: \"%s\"",
                    notif->xdg_token);

            if (notif->xdg_token == NULL)
                LOG_WARN("cannot focus window: no activation token available");
            else
                wayl_activate(term->wl, term->window, notif->xdg_token);
        }

        if (notif->activated && notif->report_activated) {
            LOG_DBG("sending notification activation event to client");

            const char *id = notif->id != NULL ? notif->id : "0";

            char button_nr[16] = {0};
            if (notif->activated_button > 0) {
                xsnprintf(
                    button_nr, sizeof(button_nr), "%u", notif->activated_button);
            }

            char reply[7 + strlen(id) + 1 + strlen(button_nr) + 2 + 1];
            size_t n = xsnprintf(
                reply, sizeof(reply), "\033]99;i=%s;%s\033\\", id, button_nr);
            term_to_slave(term, reply, n);
        }

        if (notif->report_closed) {
            LOG_DBG("sending notification close event to client");

            const char *id = notif->id != NULL ? notif->id : "0";
            char reply[7 + strlen(id) + 1 + 7 + 1 + 2 + 1];
            size_t n = xsnprintf(
                reply, sizeof(reply), "\033]99;i=%s:p=close;\033\\", id);
            term_to_slave(term, reply, n);
        }

        notify_free(term, notif);
        tll_remove(term->active_notifications, it);
        return;
    }
}

static bool
expand_action_to_argv(struct terminal *term, const char *name, const char *label,
                      size_t *argc, char ***argv)
{
    char **expanded = NULL;
    size_t count = 0;

    if (!spawn_expand_template(
        &term->conf->desktop_notifications.command_action_arg, 2,
        (const char *[]){"action-name", "action-label"},
        (const char *[]){name, label},
        &count, &expanded))
    {
        return false;
    }

    /* Append to the "global" actions argv */
    *argv = xrealloc(*argv, (*argc + count) * sizeof((*argv)[0]));
    memcpy(&(*argv)[*argc], expanded, count * sizeof(expanded[0]));
    *argc += count;

    free(expanded);
    return true;
}

bool
notify_notify(struct terminal *term, struct notification *notif)
{
    xassert(notif->xdg_token == NULL);
    xassert(notif->external_id == 0);
    xassert(notif->pid == 0);
    xassert(notif->stdout_fd <= 0);
    xassert(notif->stdout_data == NULL);
    xassert(notif->icon_path == NULL);
    xassert(notif->icon_fd <= 0);

    notif->pid = -1;
    notif->stdout_fd = -1;
    notif->icon_fd = -1;

    if (term->conf->desktop_notifications.command.argv.args == NULL)
        return false;

    if ((term->conf->desktop_notifications.inhibit_when_focused ||
         notif->when != NOTIFY_ALWAYS)
        && term->kbd_focus)
    {
        /* No notifications while we're focused */
        return false;
    }

    const char *app_id = notif->app_id != NULL
        ? notif->app_id
        : term->app_id != NULL
            ? term->app_id
            : term->conf->app_id;
    const char *title = notif->title != NULL ? notif->title : notif->body;
    const char *body = notif->title != NULL && notif->body != NULL ? notif->body : "";

    /* Icon: symbolic name if present, otherwise a filename */
    const char *icon_name_or_path = "";

    if (notif->icon_cache_id != NULL) {
        for (size_t i = 0; i < ALEN(term->notification_icons); i++) {
            const struct notification_icon *icon = &term->notification_icons[i];

            if (icon->id != NULL && streq(icon->id, notif->icon_cache_id)) {
                /* For now, we set the symbolic name to 'file:///path'
                 * when using a file based icon. */
                xassert(icon->symbolic_name != NULL);
                icon_name_or_path = icon->symbolic_name;

                LOG_DBG("using icon from cache (cache ID: %s): %s",
                        icon->id, icon_name_or_path);
                break;
            }
        }
    } else if (notif->icon_symbolic_name != NULL) {
        icon_name_or_path = notif->icon_symbolic_name;
        LOG_DBG("using symbolic icon from notification: %s", icon_name_or_path);
    } else if (notif->icon_data_sz > 0) {
        xassert(notif->icon_data != NULL);

        if (write_icon_file(
            notif->icon_data, notif->icon_data_sz,
            &notif->icon_fd,
            &notif->icon_path,
            &notif->icon_symbolic_name))
            icon_name_or_path = notif->icon_symbolic_name;

        LOG_DBG("using icon data from notification: %s", icon_name_or_path);
    }

    bool track_notification = notif->focus ||
                              notif->report_activated ||
                              notif->may_be_programatically_closed;

    uint32_t replaces_id = 0;
    if (notif->id != NULL) {
        tll_foreach(term->active_notifications, it) {
            struct notification *existing = &it->item;

            if (existing->id == NULL)
                continue;

            /*
             * When replacing/updating a notification, we may have
             * *multiple* notification helpers running for the "same"
             * notification. Make sure only the *last* notification's
             * report closed/activated are honored, to avoid sending
             * multiple reports.
             *
             * This also means we cannot 'break' out of the loop - we
             * must check *all* notifications.
             */
            if (existing->external_id != 0 && streq(existing->id, notif->id)) {
                replaces_id = existing->external_id;
                existing->report_activated = false;
                existing->report_closed = false;
            }
        }
    }

    char replaces_id_str[16];
    xsnprintf(replaces_id_str, sizeof(replaces_id_str), "%u", replaces_id);

    const char *urgency_str =
        notif->urgency == NOTIFY_URGENCY_LOW
            ? "low"
            : notif->urgency == NOTIFY_URGENCY_NORMAL
                ? "normal" : "critical";

    LOG_DBG("notify: title=\"%s\", body=\"%s\", app-id=\"%s\", category=\"%s\", "
            "urgency=\"%s\", icon=\"%s\", expires=%d, replaces=%u, muted=%s, "
            "sound-name=%s (tracking: %s)",
            title, body, app_id, notif->category, urgency_str, icon_name_or_path,
            notif->expire_time, replaces_id,
            notif->muted ? "yes" : "no", notif->sound_name,
            track_notification ? "yes" : "no");

    xassert(title != NULL);
    if (title == NULL)
        return false;

    char **argv = NULL;
    size_t argc = 0;
    char **action_argv = NULL;
    size_t action_argc = 0;

    char expire_time[16];
    xsnprintf(expire_time, sizeof(expire_time), "%d", notif->expire_time);

    if (term->conf->desktop_notifications.command_action_arg.argv.args) {
        if (!expand_action_to_argv(
            term, "default", "Activate", &action_argc, &action_argv))
        {
            return false;
        }

        size_t action_idx = 1;
        tll_foreach(notif->actions, it) {

            /* Custom actions use a numerical name, starting at 1 */
            char name[16];
            xsnprintf(name, sizeof(name), "%zu", action_idx++);

            if (!expand_action_to_argv(
                term, name, it->item, &action_argc, &action_argv))
            {
                for (size_t i = 0; i < action_argc; i++)
                    free(action_argv[i]);
                free(action_argv);
                return false;
            }
        }
    }

    if (!spawn_expand_template(
        &term->conf->desktop_notifications.command, 12,
        (const char *[]){
            "app-id", "window-title", "icon", "title", "body", "category",
            "urgency", "muted", "sound-name", "expire-time", "replace-id",
            "action-argument"},
        (const char *[]){
            app_id, term->window_title, icon_name_or_path, title, body,
            notif->category != NULL ? notif->category : "", urgency_str,
            notif->muted ? "true" : "false",
            notif->sound_name != NULL ? notif->sound_name : "",
            expire_time, replaces_id_str,

            /* Custom expansion below, since we need to expand to multiple arguments */
            "${action-argument}"},
        &argc, &argv))
    {
        return false;
    }

    /* Post-process the expanded argv, and patch in all the --action
       arguments we expanded earlier */
    for (size_t i = 0; i < argc; i++) {
        if (!streq(argv[i], "${action-argument}"))
            continue;

        if (action_argc == 0) {
            free(argv[i]);

            /* Remove ${command-argument}, but include terminating NULL */
            memmove(&argv[i], &argv[i + 1], (argc - i) * sizeof(argv[0]));
            argc--;
            break;
        }

        /* Remove the "${action-argument}" entry, add all actions argument
           from earlier, but include terminating NULL */
        argv = xrealloc(argv, (argc + action_argc) * sizeof(argv[0]));

        /* Move remaining arguments to after the action arguments */
        memmove(&argv[i + action_argc],
                &argv[i + 1],
                (argc - i) * sizeof(argv[0]));  /* Include terminating NULL */

        free(argv[i]); /* Free xstrdup("${action-argument}"); */

        /* Insert the action arguments */
        for (size_t j = 0; j < action_argc; j++) {
            argv[i + j] = action_argv[j];
            action_argv[j] = NULL;
        }

        argc += action_argc;
        argc--;  /* The ${action-argument} option has been removed */
        break;
    }

    LOG_DBG("notify command:");
    for (size_t i = 0; i < argc; i++)
        LOG_DBG("  argv[%zu] = \"%s\"", i, argv[i]);
    xassert(argv[argc] == NULL);

    int stdout_fds[2] = {-1, -1};
    if (track_notification) {
        if (pipe2(stdout_fds, O_CLOEXEC | O_NONBLOCK) < 0) {
            LOG_WARN("failed to create stdout pipe");
            track_notification = false;
            /* Non-fatal */
        } else {
            tll_push_back(term->active_notifications, *notif);

            /* We've taken over ownership of all data; clear, so that
               notify_free() doesn't double free */
            notif->id = NULL;
            notif->title = NULL;
            notif->body = NULL;
            notif->category = NULL;
            notif->app_id = NULL;
            notif->icon_cache_id = NULL;
            notif->icon_symbolic_name = NULL;
            notif->icon_data = NULL;
            notif->icon_data_sz = 0;
            notif->icon_path = NULL;
            notif->sound_name = NULL;
            notif->icon_fd = -1;
            notif->stdout_fd = -1;
            struct notification *new_notif = &tll_back(term->active_notifications);

            /* We don't need these anymore. They'll be free:d by the caller */
            new_notif->button_count = tll_length(notif->actions);
            memset(&new_notif->actions, 0, sizeof(new_notif->actions));
            notif = new_notif;
        }
    }

    if (stdout_fds[0] >= 0) {
        fdm_add(term->fdm, stdout_fds[0], EPOLLIN,
                &fdm_notify_stdout, (void *)term);
    }

    /* Redirect stdin to /dev/null, but ignore failure to open */
    int devnull = open("/dev/null", O_RDONLY);
    pid_t pid = spawn(
        term->reaper, NULL, argv, devnull, stdout_fds[1], -1,
        track_notification ? &notif_done : NULL, (void *)term, NULL);

    if (stdout_fds[1] >= 0) {
        /* Close write-end of stdout pipe */
        close(stdout_fds[1]);
    }

    if (pid < 0 && stdout_fds[0] >= 0) {
        /* Remove FDM callback if we failed to spawn */
        fdm_del(term->fdm, stdout_fds[0]);
    }

    if (devnull >= 0)
        close(devnull);

    for (size_t i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
    for (size_t i = 0; i < action_argc; i++)
        free(action_argv[i]);
    free(action_argv);

    notif->pid = pid;
    notif->stdout_fd = stdout_fds[0];
    return true;
}

void
notify_close(struct terminal *term, const char *id)
{
    xassert(id != NULL);
    LOG_DBG("close notification %s", id);

    tll_foreach(term->active_notifications, it) {
        const struct notification *notif = &it->item;
        if (notif->id == NULL || !streq(notif->id, id))
            continue;

        if (term->conf->desktop_notifications.close.argv.args == NULL) {
            LOG_DBG(
                "trying to close notification \"%s\" by sending SIGINT to %u",
                id, notif->pid);

            if (notif->pid == 0) {
                LOG_WARN(
                    "cannot close notification \"%s\": no helper process running",
                    id);
            } else {
                /* Best-effort... */
                kill(notif->pid, SIGINT);
            }
        } else {
            LOG_DBG(
                "trying to close notification \"%s\" "
                "by running user defined command", id);

            if (notif->external_id == 0) {
                LOG_WARN("cannot close notification \"%s\": "
                         "no daemon assigned notification ID available", id);
                return;
            }

            char **argv = NULL;
            size_t argc = 0;

            char external_id[16];
            xsnprintf(external_id, sizeof(external_id), "%u", notif->external_id);

            if (!spawn_expand_template(
                &term->conf->desktop_notifications.close, 1,
                (const char *[]){"id"},
                (const char *[]){external_id},
                &argc, &argv))
            {
                return;
            }

            int devnull = open("/dev/null", O_RDONLY);
            spawn(
                term->reaper, NULL, argv, devnull, -1, -1,
                NULL, (void *)term, NULL);

            if (devnull >= 0)
                close(devnull);

            for (size_t i = 0; i < argc; i++)
                free(argv[i]);
            free(argv);
        }

        return;
    }

    LOG_WARN("cannot close notification \"%s\": no such notification", id);
}

static void
add_icon(struct notification_icon *icon, const char *id, const char *symbolic_name,
         const uint8_t *data, size_t data_sz)
{
    icon->id = xstrdup(id);
    icon->symbolic_name = symbolic_name != NULL ? xstrdup(symbolic_name) : NULL;
    icon->tmp_file_name = NULL;
    icon->tmp_file_fd = -1;

    /*
     * Dump in-line data to a temporary file. This allows us to pass
     * the filename as a parameter to notification helpers
     * (i.e. notify-send -i <path>).
     *
     * Optimization: since we always prefer (i.e. use) the symbolic
     * name if present, there's no need to create a file on disk if we
     * have a symbolic name.
     */
    if (symbolic_name == NULL && data_sz > 0) {
        write_icon_file(
            data, data_sz,
            &icon->tmp_file_fd,
            &icon->tmp_file_name,
            &icon->symbolic_name);
    }

    LOG_DBG("added icon to cache: ID=%s: sym=%s, file=%s",
            icon->id, icon->symbolic_name, icon->tmp_file_name);
}

void
notify_icon_add(struct terminal *term, const char *id,
                const char *symbolic_name, const uint8_t *data, size_t data_sz)
{
#if defined(_DEBUG)
    for (size_t i = 0; i < ALEN(term->notification_icons); i++) {
        struct notification_icon *icon = &term->notification_icons[i];
        if (icon->id != NULL && streq(icon->id, id)) {
            BUG("notification icon cache already contains \"%s\"", id);
        }
    }
#endif

    for (size_t i = 0; i < ALEN(term->notification_icons); i++) {
        struct notification_icon *icon = &term->notification_icons[i];
        if (icon->id == NULL) {
            add_icon(icon, id, symbolic_name, data, data_sz);
            return;
        }
    }

    /* Cache full - throw out first entry, add new entry last */
    notify_icon_free(&term->notification_icons[0]);
    memmove(&term->notification_icons[0],
            &term->notification_icons[1],
            ((ALEN(term->notification_icons) - 1) *
             sizeof(term->notification_icons[0])));

    add_icon(
        &term->notification_icons[ALEN(term->notification_icons) - 1],
        id, symbolic_name, data, data_sz);
}

void
notify_icon_del(struct terminal *term, const char *id)
{
    for (size_t i = 0; i < ALEN(term->notification_icons); i++) {
        struct notification_icon *icon = &term->notification_icons[i];

        if (icon->id == NULL || !streq(icon->id, id))
            continue;

        LOG_DBG("expelled %s from the notification icon cache", icon->id);
        notify_icon_free(icon);
        return;
    }
}

void
notify_icon_free(struct notification_icon *icon)
{
    if (icon->tmp_file_name != NULL) {
        unlink(icon->tmp_file_name);
        if (icon->tmp_file_fd >= 0)
            close(icon->tmp_file_fd);
    }

    free(icon->id);
    free(icon->symbolic_name);
    free(icon->tmp_file_name);

    icon->id = NULL;
    icon->symbolic_name = NULL;
    icon->tmp_file_name = NULL;
    icon->tmp_file_fd = -1;
}
