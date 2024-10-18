#include "misc.h"
#include "char32.h"

bool
isword(char32_t wc, bool spaces_only, const char32_t *delimiters)
{
    if (spaces_only)
        return isc32graph(wc);

    if (c32chr(delimiters, wc) != NULL)
        return false;

    return isc32graph(wc);
}

void
timespec_add(const struct timespec *a, const struct timespec *b,
             struct timespec *res)
{
    const long one_sec_in_ns = 1000000000;

    res->tv_sec = a->tv_sec + b->tv_sec;
    res->tv_nsec = a->tv_nsec + b->tv_nsec;
    /* tv_nsec may be negative */
    if (res->tv_nsec >= one_sec_in_ns) {
        res->tv_sec++;
        res->tv_nsec -= one_sec_in_ns;
    }
}

void
timespec_sub(const struct timespec *a, const struct timespec *b,
             struct timespec *res)
{
    const long one_sec_in_ns = 1000000000;

    res->tv_sec = a->tv_sec - b->tv_sec;
    res->tv_nsec = a->tv_nsec - b->tv_nsec;
    /* tv_nsec may be negative */
    if (res->tv_nsec < 0) {
        res->tv_sec--;
        res->tv_nsec += one_sec_in_ns;
    }
}

bool
is_valid_utf8(const char *value)
{
    return value != NULL &&
           mbsntoc32(NULL, value, strlen(value), 0) != (size_t)-1;
}
