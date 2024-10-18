#include "slave.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#define LOG_MODULE "slave"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "debug.h"
#include "macros.h"
#include "tokenize.h"
#include "util.h"
#include "xmalloc.h"

extern char **environ;

struct environ {
    size_t count;
    char **envp;
};

#if !defined(EXECVPE)
static char *
find_file_in_path(const char *file)
{
    if (strchr(file, '/') != NULL)
        return xstrdup(file);

    const char *env_path = getenv("PATH");
    char *path_list = NULL;

    if (env_path != NULL && env_path[0] != '\0')
        path_list = xstrdup(env_path);
    else {
        size_t sc_path_len = confstr(_CS_PATH, NULL, 0);
        if (sc_path_len > 0) {
            path_list = xmalloc(sc_path_len);
            confstr(_CS_PATH, path_list, sc_path_len);
        } else
            return xstrdup(file);
    }

    for (const char *path = strtok(path_list, ":");
         path != NULL;
         path = strtok(NULL, ":"))
    {
        char *full = xstrjoin3(path, "/", file);
        if (access(full, F_OK) == 0) {
            free(path_list);
            return full;
        }

        free(full);
    }

    free(path_list);
    return xstrdup(file);
}

static int
foot_execvpe(const char *file, char *const argv[], char *const envp[])
{
    char *path = find_file_in_path(file);
    int ret = execve(path, argv, envp);

    /*
     * Getting here is an error
     */
    free(path);
    return ret;
}

#else   /* EXECVPE */

#define foot_execvpe(file, argv, envp) execvpe(file, argv, envp)

#endif  /* EXECVPE */

static bool
is_valid_shell(const char *shell)
{
    FILE *f = fopen("/etc/shells", "r");
    if (f == NULL)
        goto err;

    char *_line = NULL;
    size_t count = 0;

    while (true) {
        errno = 0;
        ssize_t ret = getline(&_line, &count, f);

        if (ret < 0) {
            free(_line);
            break;
        }

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

        if (line[0] == '#')
            continue;

        if (streq(line, shell)) {
            fclose(f);
            return true;
        }
    }

err:
    if (f != NULL)
        fclose(f);
    return false;
}

enum user_notification_ret_t {UN_OK, UN_NO_MORE, UN_FAIL};

static enum user_notification_ret_t
emit_one_notification(int fd, const struct user_notification *notif)
{
    const char *prefix = NULL;
    const char *postfix = "\033[m\n";

    switch (notif->kind) {
    case USER_NOTIFICATION_DEPRECATED:
        prefix = "\033[33;1mdeprecated\033[39;22m: ";
        break;

    case USER_NOTIFICATION_WARNING:
        prefix = "\033[33;1mwarning\033[39;22m: ";
        break;

    case USER_NOTIFICATION_ERROR:
        prefix = "\033[31;1merror\033[39;22m: ";
        break;
    }

    xassert(prefix != NULL);

    if (write(fd, prefix, strlen(prefix)) < 0 ||
        write(fd, "foot: ", 6) < 0 ||
        write(fd, notif->text, strlen(notif->text)) < 0 ||
        write(fd, postfix, strlen(postfix)) < 0)
    {
        /*
         * The main process is blocking and waiting for us to close
         * the error pipe. Thus, pts data will *not* be processed
         * until we've exec:d. This means we cannot write anymore once
         * the kernel buffer is full. Don't treat this as a fatal
         * error.
         */
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return UN_NO_MORE;
        else {
            LOG_ERRNO("failed to write user-notification");
            return UN_FAIL;
        }
    }

    return UN_OK;
}

static bool
emit_notifications_of_kind(int fd,
                           const user_notifications_t *notifications,
                           enum user_notification_kind kind)
{
    tll_foreach(*notifications, it) {
        if (it->item.kind == kind) {
            switch (emit_one_notification(fd, &it->item)) {
            case UN_OK:
                break;
            case UN_NO_MORE:
                return true;
            case UN_FAIL:
                return false;
            }
        }
    }

    return true;
}

static bool
emit_notifications(int fd, const user_notifications_t *notifications)
{
    return
        emit_notifications_of_kind(fd, notifications, USER_NOTIFICATION_ERROR) &&
        emit_notifications_of_kind(fd, notifications, USER_NOTIFICATION_WARNING) &&
        emit_notifications_of_kind(fd, notifications, USER_NOTIFICATION_DEPRECATED);
}

static noreturn void
slave_exec(int ptmx, char *argv[], char *const envp[], int err_fd,
           bool login_shell, const user_notifications_t *notifications)
{
    int pts = -1;
    const char *pts_name = ptsname(ptmx);

    if (grantpt(ptmx) == -1) {
        LOG_ERRNO("failed to grantpt()");
        goto err;
    }
    if (unlockpt(ptmx) == -1) {
        LOG_ERRNO("failed to unlockpt()");
        goto err;
    }

    close(ptmx);
    ptmx = -1;

    if (setsid() == -1) {
        LOG_ERRNO("failed to setsid()");
        goto err;
    }

    pts = open(pts_name, O_RDWR);
    if (pts == -1) {
        LOG_ERRNO("failed to open pseudo terminal slave device");
        goto err;
    }

    if (ioctl(pts, TIOCSCTTY, 0) < 0) {
        LOG_ERRNO("failed to configure controlling terminal");
        goto err;
    }

#ifdef IUTF8
    {
        struct termios flags;
        if (tcgetattr(pts, &flags) < 0) {
            LOG_ERRNO("failed to get terminal attributes");
            goto err;
        }

        flags.c_iflag |= IUTF8;
        if (tcsetattr(pts, TCSANOW, &flags) < 0) {
            LOG_ERRNO("failed to set IUTF8 terminal attribute");
            goto err;
        }
    }
#endif

    if (tll_length(*notifications) > 0) {
        int flags = fcntl(pts, F_GETFL);
        if (flags < 0)
            goto err;
        if (fcntl(pts, F_SETFL, flags | O_NONBLOCK) < 0)
            goto err;

        if (!emit_notifications(pts, notifications))
            goto err;

        fcntl(pts, F_SETFL, flags);
    }

    if (dup2(pts, STDIN_FILENO) == -1 ||
        dup2(pts, STDOUT_FILENO) == -1 ||
        dup2(pts, STDERR_FILENO) == -1)
    {
        LOG_ERRNO("failed to dup stdin/stdout/stderr");
        goto err;
    }

    close(pts);
    pts = -1;

    const char *file;
    if (login_shell) {
        file = xstrdup(argv[0]);

        char *arg0 = xmalloc(strlen(argv[0]) + 1 + 1);
        arg0[0] = '-';
        arg0[1] = '\0';
        strcat(arg0, argv[0]);

        argv[0] = arg0;
    } else
        file = argv[0];

    foot_execvpe(file, argv, envp);

err:
    (void)!write(err_fd, &errno, sizeof(errno));
    if (pts != -1)
        close(pts);
    if (ptmx != -1)
        close(ptmx);
    close(err_fd);
    _exit(errno);
}

static bool
env_matches_var_name(const char *e, const char *name)
{
    const size_t e_len = strlen(e);
    const size_t name_len = strlen(name);

    if (e_len <= name_len)
        return false;
    if (memcmp(e, name, name_len) != 0)
        return false;
    if (e[name_len] != '=')
        return false;
    return true;
}

static void
add_to_env(struct environ *env, const char *name, const char *value)
{
    if (env->envp == NULL)
        setenv(name, value, 1);
    else {
        char *e = xstrjoin3(name, "=", value);

        /* Search for existing variable. If found, replace it with the
           new value */
        for (size_t i = 0; i < env->count; i++) {
            if (env_matches_var_name(env->envp[i], name)) {
                free(env->envp[i]);
                env->envp[i] = e;
                return;
            }
        }

        /* If the variable does not already exist, add it */
        env->envp = xrealloc(env->envp, (env->count + 2) * sizeof(env->envp[0]));
        env->envp[env->count++] = e;
        env->envp[env->count] = NULL;
    }
}

static void
del_from_env(struct environ *env, const char *name)
{
    if (env->envp == NULL)
        unsetenv(name);
    else {
        for (size_t i = 0; i < env->count; i++) {
            if (env_matches_var_name(env->envp[i], name)) {
                free(env->envp[i]);
                memmove(&env->envp[i],
                        &env->envp[i + 1],
                        (env->count - i) * sizeof(env->envp[0]));
                env->count--;
                xassert(env->envp[env->count] == NULL);
                break;
            }
        }
    }
}

pid_t
slave_spawn(int ptmx, int argc, const char *cwd, char *const *argv,
            const char *const *envp, const env_var_list_t *extra_env_vars,
            const char *term_env, const char *conf_shell, bool login_shell,
            const user_notifications_t *notifications)
{
    int fork_pipe[2];
    if (pipe2(fork_pipe, O_CLOEXEC) < 0) {
        LOG_ERRNO("failed to create pipe");
        return -1;
    }

    pid_t pid = fork();
    switch (pid) {
    case -1:
        LOG_ERRNO("failed to fork");
        close(fork_pipe[0]);
        close(fork_pipe[1]);
        return -1;

    case 0:
        /* Child */
        close(fork_pipe[0]);  /* Close read end */

        if (chdir(cwd) < 0) {
            const int errno_copy = errno;
            LOG_ERRNO("failed to change working directory to %s", cwd);
            (void)!write(fork_pipe[1], &errno_copy, sizeof(errno_copy));
            _exit(errno_copy);
        }

        /* Restore signal mask, and SIG_IGN'd signals */
        struct sigaction dfl = {.sa_handler = SIG_DFL};
        sigemptyset(&dfl.sa_mask);
        sigset_t mask;
        sigemptyset(&mask);

        if (sigprocmask(SIG_SETMASK, &mask, NULL) < 0 ||
            sigaction(SIGHUP, &dfl, NULL) < 0 ||
            sigaction(SIGPIPE, &dfl, NULL) < 0)
        {
            const int errno_copy = errno;
            LOG_ERRNO_P(errno, "failed to restore signals");
            (void)!write(fork_pipe[1], &errno_copy, sizeof(errno_copy));
            _exit(errno_copy);
        }

        /* Create a mutable copy of the environment */
        struct environ custom_env = {0};
        if (envp != NULL) {
            for (const char *const *e = envp; *e != NULL; e++)
                custom_env.count++;

            custom_env.envp = xcalloc(
                custom_env.count + 1, sizeof(custom_env.envp[0]));

            size_t i = 0;
            for (const char *const *e = envp; *e != NULL; e++, i++)
                custom_env.envp[i] = xstrdup(*e);
            xassert(custom_env.envp[custom_env.count] == NULL);
        }

        add_to_env(&custom_env, "TERM", term_env);
        add_to_env(&custom_env, "COLORTERM", "truecolor");
        add_to_env(&custom_env, "PWD", cwd);

        del_from_env(&custom_env, "TERM_PROGRAM");
        del_from_env(&custom_env, "TERM_PROGRAM_VERSION");

#if defined(FOOT_TERMINFO_PATH)
        add_to_env(&custom_env, "TERMINFO", FOOT_TERMINFO_PATH);
#endif

        if (extra_env_vars != NULL) {
            tll_foreach(*extra_env_vars, it) {
                const char *name = it->item.name;
                const char *value = it->item.value;

                if (strlen(value) == 0)
                    del_from_env(&custom_env, name);
                else
                    add_to_env(&custom_env, name, value);
            }
        }

        char **_shell_argv = NULL;
        char **shell_argv = NULL;

        if (argc == 0) {
            if (!tokenize_cmdline(conf_shell, &_shell_argv)) {
                (void)!write(fork_pipe[1], &errno, sizeof(errno));
                _exit(0);
            }
            shell_argv = _shell_argv;
        } else {
            size_t count = 0;
            for (; argv[count] != NULL; count++)
                ;
            shell_argv = xmalloc((count + 1) * sizeof(shell_argv[0]));
            for (size_t i = 0; i < count; i++)
                shell_argv[i] = argv[i];
            shell_argv[count] = NULL;
        }

        if (is_valid_shell(shell_argv[0]))
            add_to_env(&custom_env, "SHELL", shell_argv[0]);

        slave_exec(ptmx, shell_argv,
                   custom_env.envp != NULL ? custom_env.envp : environ,
                   fork_pipe[1], login_shell, notifications);
        BUG("Unexpected return from slave_exec()");
        break;

    default: {

        /*
         * Don't stay in CWD, since it may be an ephemeral path. For
         * example, it may be a mount point of, say, a thumb drive. Us
         * keeping it open will prevent the user from unmounting it.
         */
        (void)!!chdir("/");

        close(fork_pipe[1]); /* Close write end */
        LOG_DBG("slave has PID %d", pid);

        int errno_copy;
        static_assert(sizeof(errno) == sizeof(errno_copy), "errno size mismatch");

        ssize_t ret = read(fork_pipe[0], &errno_copy, sizeof(errno_copy));
        close(fork_pipe[0]);

        if (ret < 0) {
            LOG_ERRNO("failed to read from pipe");
            return -1;
        } else if (ret == sizeof(errno_copy)) {
            LOG_ERRNO_P(
                errno_copy, "%s: failed to execute",
                argc == 0 ? conf_shell : argv[0]);
            return -1;
        } else
            LOG_DBG("%s: successfully started", conf_shell);

        int fd_flags;
        if ((fd_flags = fcntl(ptmx, F_GETFD)) < 0 ||
            fcntl(ptmx, F_SETFD, fd_flags | FD_CLOEXEC) < 0)
        {
            LOG_ERRNO("failed to set FD_CLOEXEC on ptmx");
            return -1;
        }
        break;
    }
    }

    return pid;
}
