#include "xsnprintf.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include "debug.h"
#include "macros.h"

/*
 * ISO C doesn't require vsnprintf(3) to set errno on failure, but
 * POSIX does:
 *
 * "If an output error was encountered, these functions shall return
 * a negative value and set errno to indicate the error."
 *
 * The mandated errors of interest are:
 *
 * - EILSEQ: A wide-character code does not correspond to a valid character
 * - EOVERFLOW: The value of n is greater than INT_MAX
 * - EOVERFLOW: The value to be returned is greater than INT_MAX
 *
 * ISO C11 states:
 *
 * "The vsnprintf function returns the number of characters that would
 * have been written had n been sufficiently large, not counting the
 * terminating null character, or a negative value if an encoding error
 * occurred. Thus, the null-terminated output has been completely
 * written if and only if the returned value is nonnegative and less
 * than n."
 *
 * See also:
 *
 * - ISO C11 §7.21.6.12p3
 * - https://pubs.opengroup.org/onlinepubs/9699919799/functions/vsnprintf.html
 * - https://pubs.opengroup.org/onlinepubs/9699919799/functions/snprintf.html
 */
static size_t
xvsnprintf(char *restrict buf, size_t n, const char *restrict format, va_list ap)
{
    int len = vsnprintf(buf, n, format, ap);
    if (unlikely(len < 0 || len >= (int)n)) {
        FATAL_ERROR(__func__, (len < 0) ? errno : ENOBUFS);
    }
    return (size_t)len;
}

size_t
xsnprintf(char *restrict buf, size_t n, const char *restrict format, ...)
{
    va_list ap;
    va_start(ap, format);
    size_t len = xvsnprintf(buf, n, format, ap);
    va_end(ap);
    return len;
}
