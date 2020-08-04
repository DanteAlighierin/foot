#pragma once

#include <stddef.h>
#include <string.h>
#include "macros.h"

void *xmalloc(size_t size) XMALLOC;
void *xcalloc(size_t nmemb, size_t size) XMALLOC;
char *xstrdup(const char *str) XSTRDUP;
char *xasprintf(const char *format, ...) PRINTF(1) XMALLOC;
