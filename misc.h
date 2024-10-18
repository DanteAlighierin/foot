#pragma once

#include <stdbool.h>
#include <uchar.h>
#include <time.h>

bool isword(char32_t wc, bool spaces_only, const char32_t *delimiters);

void timespec_add(const struct timespec *a, const struct timespec *b, struct timespec *res);
void timespec_sub(const struct timespec *a, const struct timespec *b, struct timespec *res);

bool is_valid_utf8(const char *value);
