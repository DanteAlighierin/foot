#pragma once

#include <stdarg.h>
#include <stddef.h>
#include "macros.h"

size_t xsnprintf(char *buf, size_t len, const char *fmt, ...) PRINTF(3) NONNULL_ARGS;
size_t xvsnprintf(char *buf, size_t len, const char *fmt, va_list ap) VPRINTF(3) NONNULL_ARGS;
