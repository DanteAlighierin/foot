#pragma once

#include <stdbool.h>
#include <uchar.h>
#include <time.h>

bool isword(char32_t wc, bool spaces_only, const char32_t *delimiters);

void timespec_sub(const struct timespec *a, const struct timespec *b, struct timespec *res);
