#pragma once

#include <stdbool.h>
#include <wchar.h>

bool isword(wchar_t wc, bool spaces_only, const wchar_t *delimiters);
