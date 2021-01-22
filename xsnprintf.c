#include "xsnprintf.h"

#include <limits.h>
#include <stdio.h>
#include "debug.h"

size_t
xvsnprintf(char *buf, size_t n, const char *format, va_list ap)
{
    xassert(n <= INT_MAX);
    int len = vsnprintf(buf, n, format, ap);

    /*
     * ISO C11 §7.21.6.5 states:
     * "The snprintf function returns the number of characters that
     * would have been written had n been sufficiently large, not
     * counting the terminating null character, or a negative value
     * if an encoding error occurred. Thus, the null-terminated output
     * has been completely written if and only if the returned value
     * is nonnegative and less than n."
     */
    xassert(len >= 0);
    xassert(len < (int)n);

    return (size_t)len;
}

size_t
xsnprintf(char *buf, size_t n, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    size_t len = xvsnprintf(buf, n, format, ap);
    va_end(ap);
    return len;
}
