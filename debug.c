#include "debug.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#if defined(__SANITIZE_ADDRESS__) || HAS_FEATURE(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define ASAN_ENABLED 1
#endif

static void
print_stack_trace(void)
{
#ifdef ASAN_ENABLED
    fputs("\nStack trace:\n", stderr);
    __sanitizer_print_stack_trace();
#endif
}

noreturn void
fatal_error(const char *msg, int err)
{
    syslog(LOG_ERR, "%s: %s", msg, strerror(err));
    errno = err;
    perror(msg);
    print_stack_trace();
    abort();
}

noreturn void
bug(const char *file, int line, const char *func, const char *fmt, ...)
{
    fprintf(stderr, "\n%s:%d: BUG in %s(): ", file, line, func);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
    print_stack_trace();
    fflush(stderr);
    abort();
}
