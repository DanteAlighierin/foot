#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xmalloc.h"
#include "debug.h"

static void *
check_alloc(void *alloc)
{
    if (unlikely(alloc == NULL)) {
        FATAL_ERROR(__func__, ENOMEM);
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
    xassert(size != 0);
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

char *
xstrndup(const char *str, size_t n)
{
    return check_alloc(strndup(str, n));
}

char32_t *
xc32dup(const char32_t *str)
{
    return check_alloc(c32dup(str));
}

static VPRINTF(2) int
xvasprintf_(char **strp, const char *format, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, format, ap2);
    if (unlikely(n < 0)) {
        FATAL_ERROR("vsnprintf", EILSEQ);
    }
    va_end(ap2);
    *strp = xmalloc(n + 1);
    return vsnprintf(*strp, n + 1, format, ap);
}

char *
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
