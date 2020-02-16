#include "log.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>

#include <syslog.h>

static bool colorize = false;
static bool do_syslog = true;

void
log_init(enum log_colorize _colorize, bool _do_syslog,
         enum log_facility syslog_facility, enum log_class syslog_level)
{
    static const int facility_map[] = {
        [LOG_FACILITY_USER] = LOG_USER,
        [LOG_FACILITY_DAEMON] = LOG_DAEMON,
    };

    static const int level_map[] = {
        [LOG_CLASS_ERROR] = LOG_ERR,
        [LOG_CLASS_WARNING] = LOG_WARNING,
        [LOG_CLASS_INFO] = LOG_INFO,
        [LOG_CLASS_DEBUG] = LOG_DEBUG,
    };

    colorize = _colorize == LOG_COLORIZE_NEVER ? false : _colorize == LOG_COLORIZE_ALWAYS ? true : isatty(STDERR_FILENO);
    do_syslog = _do_syslog;

    if (do_syslog) {
        openlog(NULL, /*LOG_PID*/0, facility_map[syslog_facility]);
        setlogmask(LOG_UPTO(level_map[syslog_level]));
    }
}

void
log_deinit(void)
{
    if (do_syslog)
        closelog();
}

static void
_log(enum log_class log_class, const char *module, const char *file, int lineno,
     const char *fmt, int sys_errno, va_list va)
{
    const char *class = "abcd";
    int class_clr = 0;
    switch (log_class) {
    case LOG_CLASS_ERROR:    class = " err"; class_clr = 31; break;
    case LOG_CLASS_WARNING:  class = "warn"; class_clr = 33; break;
    case LOG_CLASS_INFO:     class = "info"; class_clr = 97; break;
    case LOG_CLASS_DEBUG:    class = " dbg"; class_clr = 36; break;
    }

    char clr[16];
    snprintf(clr, sizeof(clr), "\e[%dm", class_clr);
    fprintf(stderr, "%s%s%s: ", colorize ? clr : "", class, colorize ? "\e[0m" : "");

    if (colorize)
        fprintf(stderr, "\e[2m");
    fprintf(stderr, "%s:%d: ", file, lineno);
    if (colorize)
        fprintf(stderr, "\e[0m");

    vfprintf(stderr, fmt, va);

    if (sys_errno != 0)
        fprintf(stderr, ": %s", strerror(sys_errno));

    fprintf(stderr, "\n");
}

static void
_sys_log(enum log_class log_class, const char *module,
         const char *file __attribute__((unused)),
         int lineno __attribute__((unused)),
         const char *fmt, int sys_errno, va_list va)
{
    if (!do_syslog)
        return;

    /* Map our log level to syslog's level */
    int level = -1;
    switch (log_class) {
    case LOG_CLASS_ERROR:    level = LOG_ERR; break;
    case LOG_CLASS_WARNING:  level = LOG_WARNING; break;
    case LOG_CLASS_INFO:     level = LOG_INFO; break;
    case LOG_CLASS_DEBUG:    level = LOG_DEBUG; break;
    }

    assert(level != -1);

    const char *sys_err = sys_errno != 0 ? strerror(sys_errno) : NULL;

    va_list va2;
    va_copy(va2, va);

    /* Calculate required size of buffer holding the entire log message */
    int required_len = 0;
    required_len += strlen(module) + 2;  /* "%s: " */
    required_len += vsnprintf(NULL, 0, fmt, va2); va_end(va2);

    if (sys_errno != 0)
        required_len += strlen(sys_err) + 2; /* ": %s" */

    /* Format the msg */
    char *msg = malloc(required_len + 1);
    int idx = 0;

    idx += snprintf(&msg[idx], required_len + 1 - idx, "%s: ", module);
    idx += vsnprintf(&msg[idx], required_len + 1 - idx, fmt, va);

    if (sys_errno != 0) {
        snprintf(
            &msg[idx], required_len + 1 - idx, ": %s", strerror(sys_errno));
    }

    syslog(level, "%s", msg);
    free(msg);
}

void
log_msg(enum log_class log_class, const char *module,
        const char *file, int lineno, const char *fmt, ...)
{
    va_list ap1, ap2;
    va_start(ap1, fmt);
    va_copy(ap2, ap1);
    _log(log_class, module, file, lineno, fmt, 0, ap1);
    _sys_log(log_class, module, file, lineno, fmt, 0, ap2);
    va_end(ap1);
    va_end(ap2);
}

void log_errno(enum log_class log_class, const char *module,
               const char *file, int lineno,
               const char *fmt, ...)
{
    va_list ap1, ap2;
    va_start(ap1, fmt);
    va_copy(ap2, ap1);
    _log(log_class, module, file, lineno, fmt, errno, ap1);
    _sys_log(log_class, module, file, lineno, fmt, errno, ap2);
    va_end(ap1);
    va_end(ap2);
}

void log_errno_provided(enum log_class log_class, const char *module,
                        const char *file, int lineno, int _errno,
                        const char *fmt, ...)
{
    va_list ap1, ap2;
    va_start(ap1, fmt);
    va_copy(ap2, ap1);
    _log(log_class, module, file, lineno, fmt, _errno, ap1);
    _sys_log(log_class, module, file, lineno, fmt, _errno, ap2);
    va_end(ap1);
    va_end(ap2);
}
