#pragma once
#include <stdbool.h>

enum log_colorize { LOG_COLORIZE_NEVER, LOG_COLORIZE_ALWAYS, LOG_COLORIZE_AUTO };
enum log_facility { LOG_FACILITY_USER, LOG_FACILITY_DAEMON };
enum log_class { LOG_CLASS_ERROR, LOG_CLASS_WARNING, LOG_CLASS_INFO, LOG_CLASS_DEBUG };

void log_init(enum log_colorize colorize, bool do_syslog,
              enum log_facility syslog_facility, enum log_class syslog_level);
void log_deinit(void);

void log_msg(enum log_class log_class, const char *module,
               const char *file, int lineno,
               const char *fmt, ...) __attribute__((format (printf, 5, 6)));

void log_errno(enum log_class log_class, const char *module,
               const char *file, int lineno,
               const char *fmt, ...) __attribute__((format (printf, 5, 6)));

void log_errno_provided(
    enum log_class log_class, const char *module,
    const char *file, int lineno, int _errno,
    const char *fmt, ...) __attribute__((format (printf, 6, 7)));

#define LOG_ERR(fmt, ...)  \
    log_msg(LOG_CLASS_ERROR, LOG_MODULE, __FILE__, __LINE__, fmt, ## __VA_ARGS__)
#define LOG_ERRNO(fmt, ...) \
    log_errno(LOG_CLASS_ERROR, LOG_MODULE, __FILE__, __LINE__, fmt, ## __VA_ARGS__)
#define LOG_ERRNO_P(fmt, _errno, ...)                                   \
    log_errno_provided(LOG_CLASS_ERROR, LOG_MODULE, __FILE__, __LINE__, \
                       _errno, fmt, ## __VA_ARGS__)
#define LOG_WARN(fmt, ...)  \
    log_msg(LOG_CLASS_WARNING, LOG_MODULE, __FILE__, __LINE__, fmt, ## __VA_ARGS__)
#define LOG_INFO(fmt, ...)  \
    log_msg(LOG_CLASS_INFO, LOG_MODULE, __FILE__, __LINE__, fmt, ## __VA_ARGS__)

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
 #define LOG_DBG(fmt, ...)  \
    log_msg(LOG_CLASS_DEBUG, LOG_MODULE, __FILE__, __LINE__, fmt, ## __VA_ARGS__)
#else
 #define LOG_DBG(fmt, ...)
#endif
