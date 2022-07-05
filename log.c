#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "debug.h"
#include "util.h"
#include "xsnprintf.h"

static bool colorize = false;
static bool do_syslog = true;
static enum log_class log_level = LOG_CLASS_NONE;

static const struct {
    const char name[8];
    const char log_prefix[7];
    uint8_t color;
    int syslog_equivalent;
} log_level_map[] = {
    [LOG_CLASS_NONE] = {"none", "none", 5, -1},
    [LOG_CLASS_ERROR] = {"error", " err", 31, LOG_ERR},
    [LOG_CLASS_WARNING] = {"warning", "warn", 33, LOG_WARNING},
    [LOG_CLASS_INFO] = {"info", "info", 97, LOG_INFO},
    [LOG_CLASS_DEBUG] = {"debug", " dbg", 36, LOG_DEBUG},
};

void
log_init(enum log_colorize _colorize, bool _do_syslog,
         enum log_facility syslog_facility, enum log_class _log_level)
{
    static const int facility_map[] = {
        [LOG_FACILITY_USER] = LOG_USER,
        [LOG_FACILITY_DAEMON] = LOG_DAEMON,
    };

    colorize = _colorize == LOG_COLORIZE_ALWAYS || (_colorize == LOG_COLORIZE_AUTO && isatty(STDERR_FILENO));
    do_syslog = _do_syslog;
    log_level = _log_level;

    int slvl = log_level_map[_log_level].syslog_equivalent;
    if (do_syslog && slvl != -1) {
        openlog(NULL, /*LOG_PID*/0, facility_map[syslog_facility]);
        setlogmask(LOG_UPTO(slvl));
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
    xassert(log_class > LOG_CLASS_NONE);
    xassert(log_class < ALEN(log_level_map));

    if (log_class > log_level)
        return;

    const char *prefix = log_level_map[log_class].log_prefix;
    unsigned int class_clr = log_level_map[log_class].color;

    char clr[16];
    xsnprintf(clr, sizeof(clr), "\033[%um", class_clr);
    fprintf(stderr, "%s%s%s: ", colorize ? clr : "", prefix, colorize ? "\033[0m" : "");

    if (colorize)
        fputs("\033[2m", stderr);
    fprintf(stderr, "%s:%d: ", file, lineno);
    if (colorize)
        fputs("\033[0m", stderr);

    vfprintf(stderr, fmt, va);

    if (sys_errno != 0)
        fprintf(stderr, ": %s", strerror(sys_errno));

    fputc('\n', stderr);
}

static void
_sys_log(enum log_class log_class, const char *module,
         const char UNUSED *file, int UNUSED lineno,
         const char *fmt, int sys_errno, va_list va)
{
    xassert(log_class > LOG_CLASS_NONE);
    xassert(log_class < ALEN(log_level_map));

    if (!do_syslog)
        return;

    /* Map our log level to syslog's level */
    int level = log_level_map[log_class].syslog_equivalent;

    char msg[4096];
    int n = vsnprintf(msg, sizeof(msg), fmt, va);
    xassert(n >= 0);

    if (sys_errno != 0 && (size_t)n < sizeof(msg))
        snprintf(msg + n, sizeof(msg) - n, ": %s", strerror(sys_errno));

    syslog(level, "%s: %s", module, msg);
}

void
log_msg_va(enum log_class log_class, const char *module,
           const char *file, int lineno, const char *fmt, va_list va)
{
    va_list va2;
    va_copy(va2, va);
    _log(log_class, module, file, lineno, fmt, 0, va);
    _sys_log(log_class, module, file, lineno, fmt, 0, va2);
    va_end(va2);
}

void
log_msg(enum log_class log_class, const char *module,
        const char *file, int lineno, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    log_msg_va(log_class, module, file, lineno, fmt, va);
    va_end(va);
}

void
log_errno_va(enum log_class log_class, const char *module,
             const char *file, int lineno,
             const char *fmt, va_list va)
{
    log_errno_provided_va(log_class, module, file, lineno, errno, fmt, va);
}

void
log_errno(enum log_class log_class, const char *module,
          const char *file, int lineno,
          const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    log_errno_va(log_class, module, file, lineno, fmt, va);
    va_end(va);
}

void
log_errno_provided_va(enum log_class log_class, const char *module,
                      const char *file, int lineno, int errno_copy,
                      const char *fmt, va_list va)
{
    va_list va2;
    va_copy(va2, va);
    _log(log_class, module, file, lineno, fmt, errno_copy, va);
    _sys_log(log_class, module, file, lineno, fmt, errno_copy, va2);
    va_end(va2);
}

void
log_errno_provided(enum log_class log_class, const char *module,
                   const char *file, int lineno, int errno_copy,
                   const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    log_errno_provided_va(log_class, module, file, lineno, errno_copy, fmt, va);
    va_end(va);
}

static size_t
map_len(void)
{
    size_t len = ALEN(log_level_map);
#ifndef _DEBUG
    /* Exclude "debug" entry for non-debug builds */
    len--;
#endif
    return len;
}

int
log_level_from_string(const char *str)
{
    if (unlikely(str[0] == '\0'))
        return -1;

    for (int i = 0, n = map_len(); i < n; i++)
        if (strcmp(str, log_level_map[i].name) == 0)
            return i;

    return -1;
}

const char *
log_level_string_hint(void)
{
    static char buf[64];
    if (buf[0] != '\0')
        return buf;

    for (size_t i = 0, pos = 0, n = map_len(); i < n; i++) {
        const char *entry = log_level_map[i].name;
        const char *delim = (i + 1 < n) ? ", " : "";
        pos += xsnprintf(buf + pos, sizeof(buf) - pos, "'%s'%s", entry, delim);
    }

    return buf;
}
