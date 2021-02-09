#pragma once

#include "macros.h"

#define FATAL_ERROR(...) fatal_error(__FILE__, __LINE__, __VA_ARGS__)

#ifdef NDEBUG
    #define BUG(...) UNREACHABLE()
#else
    #define BUG(...) bug(__FILE__, __LINE__, __func__, __VA_ARGS__)
#endif

#define xassert(x) do { \
    IGNORE_WARNING("-Wtautological-compare") \
    if (unlikely(!(x))) { \
        BUG("assertion failed: '%s'", #x); \
    } \
    UNIGNORE_WARNINGS \
} while (0)

#ifndef static_assert
    #if __STDC_VERSION__ >= 201112L
        #define static_assert(x, msg) _Static_assert((x), msg)
    #elif GNUC_AT_LEAST(4, 6) || HAS_EXTENSION(c_static_assert)
        #define static_assert(x, msg) __extension__ _Static_assert((x), msg)
    #else
        #define static_assert(x, msg)
    #endif
#endif

noreturn void fatal_error(const char *file, int line, const char *msg, int err) COLD;
noreturn void bug(const char *file, int line, const char *func, const char *fmt, ...) PRINTF(4) COLD;
