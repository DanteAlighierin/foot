#pragma once

#include <stdint.h>
#include <threads.h>

#define ALEN(v) (sizeof(v) / sizeof((v)[0]))
#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

static inline const char *
thrd_err_as_string(int thrd_err)
{
    switch (thrd_err) {
    case thrd_success:  return "success";
    case thrd_busy:     return "busy";
    case thrd_nomem:    return "no memory";
    case thrd_timedout: return "timedout";

    case thrd_error:
    default:            return "unknown error";
    }

    return "unknown error";
}

static inline uint64_t
sdbm_hash(const char *s)
{
    uint64_t hash = 0;

    for (; *s != '\0'; s++) {
        int c = *s;
        hash = c + (hash << 6) + (hash << 16) - hash;
    }

    return hash;
}

enum {
    HEX_DIGIT_INVALID = 16
};

static inline uint8_t
hex2nibble(char c)
{
    switch (c) {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        return c - '0';

    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
        return c - 'a' + 10;

    case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
        return c - 'A' + 10;
    }

    return HEX_DIGIT_INVALID;
}
