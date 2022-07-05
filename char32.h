#pragma once

#include <stdbool.h>
#include <uchar.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

static inline size_t c32len(const char32_t *s) {
    return wcslen((const wchar_t *)s);
}

static inline int c32cmp(const char32_t *s1, const char32_t *s2) {
    return wcscmp((const wchar_t *)s1, (const wchar_t *)s2);
}

static inline char32_t *c32ncpy(char32_t *dst, const char32_t *src, size_t n) {
    return (char32_t *)wcsncpy((wchar_t *)dst, (const wchar_t *)src, n);
}

static inline char32_t *c32cpy(char32_t *dst, const char32_t *src) {
    return (char32_t *)wcscpy((wchar_t *)dst, (const wchar_t *)src);
}

static inline char32_t *c32ncat(char32_t *dst, const char32_t *src, size_t n) {
    return (char32_t *)wcsncat((wchar_t *)dst, (const wchar_t *)src, n);
}

static inline char32_t *c32cat(char32_t *dst, const char32_t *src) {
    return (char32_t *)wcscat((wchar_t *)dst, (const wchar_t *)src);
}

static inline char32_t *c32dup(const char32_t *s) {
    return (char32_t *)wcsdup((const wchar_t *)s);
}

static inline char32_t *c32chr(const char32_t *s, char32_t c) {
    return (char32_t *)wcschr((const wchar_t *)s, c);
}

static inline int c32casecmp(const char32_t *s1, const char32_t *s2) {
    return wcscasecmp((const wchar_t *)s1, (const wchar_t *)s2);
}

static inline int c32ncasecmp(const char32_t *s1, const char32_t *s2, size_t n) {
    return wcsncasecmp((const wchar_t *)s1, (const wchar_t *)s2, n);
}

static inline char32_t toc32lower(char32_t c) {
    return (char32_t)towlower((wint_t)c);
}

static inline char32_t toc32upper(char32_t c) {
    return (char32_t)towupper((wint_t)c);
}

static inline bool isc32space(char32_t c32) {
    return iswspace((wint_t)c32);
}

static inline bool isc32print(char32_t c32) {
    return iswprint((wint_t)c32);
}

static inline bool isc32graph(char32_t c32) {
    return iswgraph((wint_t)c32);
}

static inline int c32width(char32_t c) {
    return wcwidth((wchar_t)c);
}

static inline int c32swidth(const char32_t *s, size_t n) {
    return wcswidth((const wchar_t *)s, n);
}

size_t mbsntoc32(char32_t *dst, const char *src, size_t nms, size_t len);
char32_t *ambstoc32(const char *src);
char *ac32tombs(const char32_t *src);

static inline size_t mbstoc32(char32_t *dst, const char *src, size_t len) {
    return mbsntoc32(dst, src, strlen(src) + 1, len);
}
