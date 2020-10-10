#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <syslog.h>
#include "xmalloc.h"

static NORETURN COLD void
fatal_error(const char *msg, int err)
{
    syslog(LOG_ERR, "%s: %s", msg, strerror(err));
    errno = err;
    perror(msg);
    abort();
}

static void *
check_alloc(void *alloc)
{
    if (unlikely(alloc == NULL)) {
        fatal_error(__func__, ENOMEM);
    }
    return alloc;
}

void *
xmalloc(size_t size)
{
    if (unlikely(size == 0)) {
        size = 1;
    }
    return check_alloc(malloc(size));
}

void *
xcalloc(size_t nmemb, size_t size)
{
    assert(size != 0);
    return check_alloc(calloc(likely(nmemb) ? nmemb : 1, size));
}

void *
xrealloc(void *ptr, size_t size)
{
    void *alloc = realloc(ptr, size);
    return unlikely(size == 0) ? alloc : check_alloc(alloc);
}

char *
xstrdup(const char *str)
{
    return check_alloc(strdup(str));
}

wchar_t *
xwcsdup(const wchar_t *str)
{
    return check_alloc(wcsdup(str));
}

char *
xstrndup(const char *str, size_t n)
{
    return check_alloc(strndup(str, n));
}

static VPRINTF(2) int
xvasprintf_(char **strp, const char *format, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, format, ap2);
    if (unlikely(n < 0)) {
        fatal_error("vsnprintf", EILSEQ);
    }
    va_end(ap2);
    *strp = xmalloc(n + 1);
    return vsnprintf(*strp, n + 1, format, ap);
}

static VPRINTF(1) char *
xvasprintf(const char *format, va_list ap)
{
    char *str;
    xvasprintf_(&str, format, ap);
    return str;
}

char *
xasprintf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    char *str = xvasprintf(format, ap);
    va_end(ap);
    return str;
}
