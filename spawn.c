#include "spawn.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "spawn"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "xmalloc.h"

bool
spawn(struct reaper *reaper, const char *cwd, char *const argv[],
      int stdin_fd, int stdout_fd, int stderr_fd)
{
    int pipe_fds[2] = {-1, -1};
    if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
        LOG_ERRNO("failed to create pipe");
        goto err;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERRNO("failed to fork");
        goto err;
    }

    if (pid == 0) {
        /* Child */
        close(pipe_fds[0]);

        /* Clear signal mask */
        sigset_t mask;
        sigemptyset(&mask);
        if (sigprocmask(SIG_SETMASK, &mask, NULL) < 0) {
            const int errno_copy = errno;
            LOG_ERRNO("failed to restore signals");
            (void)!write(pipe_fds[1], &errno_copy, sizeof(errno_copy));
            _exit(errno_copy);
        }

        if ((stdin_fd >= 0 && (dup2(stdin_fd, STDIN_FILENO) < 0 || close(stdin_fd) < 0)) ||
            (stdout_fd >= 0 && (dup2(stdout_fd, STDOUT_FILENO) < 0 || close(stdout_fd) < 0)) ||
            (stderr_fd >= 0 && (dup2(stderr_fd, STDERR_FILENO) < 0 || close(stderr_fd) < 0)) ||
            (cwd != NULL && chdir(cwd) < 0) ||
            execvp(argv[0], argv) < 0)
        {
            const int errno_copy = errno;
            (void)!write(pipe_fds[1], &errno_copy, sizeof(errno_copy));
            _exit(errno_copy);
        }
        xassert(false);
        _exit(errno);
    }

    /* Parent */
    close(pipe_fds[1]);

    int errno_copy;
    static_assert(sizeof(errno_copy) == sizeof(errno), "errno size mismatch");

    ssize_t ret = read(pipe_fds[0], &errno_copy, sizeof(errno_copy));
    close(pipe_fds[0]);

    if (ret == 0) {
        reaper_add(reaper, pid, NULL, NULL);
        return true;
    } else if (ret < 0) {
        LOG_ERRNO("failed to read from pipe");
        return false;
    } else {
        LOG_ERRNO_P(errno_copy, "%s: failed to spawn", argv[0]);
        errno = errno_copy;
        waitpid(pid, NULL, 0);
        return false;
    }

err:
    if (pipe_fds[0] != -1)
        close(pipe_fds[0]);
    if (pipe_fds[1] != -1)
        close(pipe_fds[1]);
    return false;
}

bool
spawn_expand_template(const struct config_spawn_template *template,
                      size_t key_count,
                      const char *key_names[static key_count],
                      const char *key_values[static key_count],
                      size_t *argc, char ***argv)
{
    *argc = 0;
    *argv = NULL;

    for (; template->argv[*argc] != NULL; (*argc)++)
        ;

#define append(s, n)                                        \
    do {                                                    \
        expanded = xrealloc(expanded, len + (n) + 1);       \
        memcpy(&expanded[len], s, n);                       \
        len += n;                                           \
        expanded[len] = '\0';                               \
    } while (0)

    *argv = malloc((*argc + 1) * sizeof((*argv)[0]));

    /* Expand the provided keys */
    for (size_t i = 0; i < *argc; i++) {
        size_t len = 0;
        char *expanded = NULL;

        char *start = NULL;
        char *last_end = template->argv[i];

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
            bool valid_key = false;
            for (size_t j = 0; j < key_count; j++) {
                if (strncmp(start, key_names[j], end - start) != 0)
                    continue;

                append(key_values[j], strlen(key_values[j]));
                valid_key = true;
                break;
            }

            if (!valid_key) {
                /* Unrecognized template - append it as-is */
                start -= 2;
                append(start, end + 1 - start);
                LOG_WARN("notify: unrecognized template: %.*s",
                         (int)(end + 1 - start), start);
            }

            last_end = end + 1;
        }

        append(last_end, template->argv[i] + strlen(template->argv[i]) - last_end);
        (*argv)[i] = expanded;
    }
    (*argv)[*argc] = NULL;

#undef append
    return true;
}
