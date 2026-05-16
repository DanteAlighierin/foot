#include "base64.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#define LOG_MODULE "base64"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "macros.h"
#include "util.h"
#include "xmalloc.h"

enum {
    P = 1 << 6, // Padding byte (=)
    I = 1 << 7, // Invalid byte ([^A-Za-z0-9+/=])
};

static const uint8_t reverse_lookup[256] = {
     I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,
     I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,
     I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I, 62,  I,  I,  I, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61,  I,  I,  I,  P,  I,  I,
     I,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,  I,  I,  I,  I,  I,
     I, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,  I,  I,  I,  I,  I,
     I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,
     I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,
     I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,
     I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,
     I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,
     I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,
     I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,
     I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I,  I
};

static const char lookup[64] = {
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/"
};

char *
base64_decode(const char *s, size_t *size)
{
    const size_t len = strlen(s);
    if (unlikely(len % 4 != 0)) {
        errno = EINVAL;
        return NULL;
    }

    char *ret = malloc(len / 4 * 3 + 1);
    if (unlikely(ret == NULL))
        return NULL;

    if (unlikely(size != NULL))
        *size = len / 4 * 3;

    for (size_t i = 0, o = 0; i < len; i += 4, o += 3) {
        unsigned a = reverse_lookup[(unsigned char)s[i + 0]];
        unsigned b = reverse_lookup[(unsigned char)s[i + 1]];
        unsigned c = reverse_lookup[(unsigned char)s[i + 2]];
        unsigned d = reverse_lookup[(unsigned char)s[i + 3]];

        unsigned u = a | b | c | d;
        if (unlikely(u & I))
            goto invalid;

        if (unlikely(u & P)) {
            if (unlikely(i + 4 != len || (a | b) & P || (c & P && !(d & P))))
                goto invalid;

            if (unlikely(size != NULL)) {
                if (c & P)
                    *size = len / 4 * 3 - 2;
                else
                    *size = len / 4 * 3 - 1;
            }

            c &= 63;
            d &= 63;
        }

        uint32_t v = a << 18 | b << 12 | c << 6 | d << 0;
        char x = (v >> 16) & 0xff;
        char y = (v >>  8) & 0xff;
        char z = (v >>  0) & 0xff;

        LOG_DBG("%c%c%c", x, y, z);
        ret[o + 0] = x;
        ret[o + 1] = y;
        ret[o + 2] = z;
    }

    ret[len / 4 * 3] = '\0';
    return ret;

invalid:
    free(ret);
    errno = EINVAL;
    return NULL;
}

static inline size_t ALWAYS_INLINE
base64_out_size_for(size_t in_size)
{
    return (in_size + 2) / 3 * 4 + 1;  /* +1 since we always NULL terminate */
}

static size_t
base64_encode_to_buffer(const uint8_t *data, size_t size, char *out)
{
    xassert(size % 3 == 0);

    for (size_t i = 0, o = 0; i < size; i += 3, o += 4) {
        int x = data[i + 0];
        int y = data[i + 1];
        int z = data[i + 2];

        uint32_t v = x << 16 | y << 8 | z << 0;

        unsigned a = (v >> 18) & 0x3f;
        unsigned b = (v >> 12) & 0x3f;
        unsigned c = (v >>  6) & 0x3f;
        unsigned d = (v >>  0) & 0x3f;

        char c0 = lookup[a];
        char c1 = lookup[b];
        char c2 = lookup[c];
        char c3 = lookup[d];

        out[o + 0] = c0;
        out[o + 1] = c1;
        out[o + 2] = c2;
        out[o + 3] = c3;

        LOG_DBG("base64: encode: %c%c%c%c", c0, c1, c2, c3);
    }

    const size_t out_len = size / 3 * 4;
    out[out_len] = '\0';
    return out_len;
}

char *
base64_encode(const void *data, size_t size)
{
    if (unlikely(size % 3 != 0))
        return NULL;

    char *ret = malloc(base64_out_size_for(size));
    if (unlikely(ret == NULL))
        return NULL;

    base64_encode_to_buffer(data, size, ret);
    return ret;
}

void
base64_encode_final(const void *_data, size_t size, char result[4])
{
    xassert(size > 0);
    xassert(size < 3);

    const uint8_t *data = _data;

    uint32_t v = 0;
    if (size >= 1)
        v |= data[0] << 16;
    if (size >= 2)
        v |= data[1] << 8;

    unsigned a = (v >> 18) & 0x3f;
    unsigned b = (v >> 12) & 0x3f;
    unsigned c = (v >>  6) & 0x3f;

    char c0 = lookup[a];
    char c1 = lookup[b];
    char c2 = size == 2 ? lookup[c] : '=';
    char c3 = '=';

    result[0] = c0;
    result[1] = c1;
    result[2] = c2;
    result[3] = c3;

    LOG_DBG("base64: encode: %c%c%c%c", c0, c1, c2, c3);
}

char *
base64_encode_oneshot(const void *data, size_t size)
{
    const size_t encoded_size = base64_out_size_for(size);

    char *encoded = malloc(encoded_size);
    if (unlikely(encoded == NULL))
        return NULL;

    size_t out_len = base64_encode_to_buffer(data, size / 3 * 3, encoded);

    const size_t remaining = size % 3;
    if (remaining == 0)
        return encoded;

    base64_encode_final(&((const uint8_t *)data)[size - remaining],
                        remaining, &encoded[out_len]);
    encoded[out_len + 4] = '\0';
    return encoded;
}

UNITTEST
{
    char *encoded = NULL;

    encoded = base64_encode("", 0);
    xassert(streq(encoded, ""));
    free(encoded);

    encoded = base64_encode_oneshot("123", 3);
    xassert(streq(encoded, "MTIz"));
    free(encoded);

    encoded = base64_encode_oneshot("123456", 6);
    xassert(streq(encoded, "MTIzNDU2"));
    free(encoded);
}

UNITTEST
{
    char *encoded = NULL;

    encoded = base64_encode_oneshot("", 0);
    xassert(streq(encoded, ""));
    free(encoded);

    encoded = base64_encode_oneshot("1", 1);
    xassert(streq(encoded, "MQ=="));
    free(encoded);

    encoded = base64_encode_oneshot("12", 2);
    xassert(streq(encoded, "MTI="));
    free(encoded);

    encoded = base64_encode_oneshot("123", 3);
    xassert(streq(encoded, "MTIz"));
    free(encoded);

    encoded = base64_encode_oneshot("1234", 4);
    xassert(streq(encoded, "MTIzNA=="));
    free(encoded);

    encoded = base64_encode_oneshot("12345", 5);
    xassert(streq(encoded, "MTIzNDU="));
    free(encoded);

    encoded = base64_encode_oneshot("123456", 6);
    xassert(streq(encoded, "MTIzNDU2"));
    free(encoded);

    encoded = base64_encode_oneshot("hello world", 11);
    xassert(streq(encoded, "aGVsbG8gd29ybGQ="));
    free(encoded);

}
