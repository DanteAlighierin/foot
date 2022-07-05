#pragma once
#include <stdbool.h>
#include <stdarg.h>
#include "macros.h"

enum log_colorize { LOG_COLORIZE_NEVER, LOG_COLORIZE_ALWAYS, LOG_COLORIZE_AUTO };
enum log_facility { LOG_FACILITY_USER, LOG_FACILITY_DAEMON };

enum log_class {
    LOG_CLASS_NONE,
    LOG_CLASS_ERROR,
    LOG_CLASS_WARNING,
    LOG_CLASS_INFO,
    LOG_CLASS_DEBUG,
    LOG_CLASS_COUNT,
};

void log_init(enum log_colorize colorize, bool do_syslog,
              enum log_facility syslog_facility, enum log_class log_level);
void log_deinit(void);

void log_msg(
    enum log_class log_class, const char *module,
    const char *file, int lineno,
    const char *fmt, ...) PRINTF(5);

void log_errno(
    enum log_class log_class, const char *module,
    const char *file, int lineno,
    const char *fmt, ...) PRINTF(5);

void log_errno_provided(
    enum log_class log_class, const char *module,
    const char *file, int lineno, int _errno,
    const char *fmt, ...) PRINTF(6);

void log_msg_va(
    enum log_class log_class, const char *module,
    const char *file, int lineno, const char *fmt, va_list va) VPRINTF(5);
void log_errno_va(
    enum log_class log_class, const char *module,
    const char *file, int lineno,
    const char *fmt, va_list va) VPRINTF(5);
void log_errno_provided_va(
    enum log_class log_class, const char *module,
    const char *file, int lineno, int _errno,
    const char *fmt, va_list va) VPRINTF(6);


int log_level_from_string(const char *str);
const char *log_level_string_hint(void);

#define LOG_ERR(...)  \
    log_msg(LOG_CLASS_ERROR, LOG_MODULE, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERRNO(...) \
    log_errno(LOG_CLASS_ERROR, LOG_MODULE, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERRNO_P(_errno, ...)                                        \
    log_errno_provided(LOG_CLASS_ERROR, LOG_MODULE, __FILE__, __LINE__, \
                       _errno, __VA_ARGS__)
#define LOG_WARN(...)  \
    log_msg(LOG_CLASS_WARNING, LOG_MODULE, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  \
    log_msg(LOG_CLASS_INFO, LOG_MODULE, __FILE__, __LINE__,  __VA_ARGS__)

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
 #define LOG_DBG(...)  \
    log_msg(LOG_CLASS_DEBUG, LOG_MODULE, __FILE__, __LINE__, __VA_ARGS__)
#else
 #define LOG_DBG(...)
#endif
