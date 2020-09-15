#pragma once

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
