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

static void __attribute__((constructor))
init(void)
{
    colorize = isatty(STDOUT_FILENO);
    openlog(NULL, /*LOG_PID*/0, LOG_USER);
    setlogmask(LOG_UPTO(LOG_WARNING));
}

static void __attribute__((destructor))
fini(void)
{
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
    printf("%s%s%s: ", colorize ? clr : "", class, colorize ? "\e[0m" : "");

    if (colorize)
        printf("\e[2m");
#if defined(_DEBUG)
    printf("%s:%d: ", file, lineno);
#else
    printf("%s: ", module);
#endif
    if (colorize)
        printf("\e[0m");

    vprintf(fmt, va);

    if (sys_errno != 0)
        printf(": %s", strerror(sys_errno));

    printf("\n");
}

static void
_sys_log(enum log_class log_class, const char *module, const char *file,
         int lineno, const char *fmt, int sys_errno, va_list va)
{
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
        idx += snprintf(
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
