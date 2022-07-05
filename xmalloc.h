#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <wchar.h>
#include <uchar.h>

#include "char32.h"
#include "macros.h"

void *xmalloc(size_t size) XMALLOC;
void *xcalloc(size_t nmemb, size_t size) XMALLOC;
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *str) XSTRDUP;
char *xstrndup(const char *str, size_t n) XSTRDUP;
char *xasprintf(const char *format, ...) PRINTF(1) XMALLOC;
char *xvasprintf(const char *format, va_list va) VPRINTF(1) XMALLOC;
char32_t *xc32dup(const char32_t *str) XSTRDUP;
