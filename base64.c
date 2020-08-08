#include "base64.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#define LOG_MODULE "base64"
#define LOG_ENABLE_DBG 0
#include "log.h"

static const uint8_t reverse_lookup[256] = {
    ['A'] = 0, ['B'] = 1, ['C'] = 2, ['D'] = 3, ['E'] = 4, ['F'] = 5, ['G'] = 6,
    ['H'] = 7, ['I'] = 8, ['J'] = 9, ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13,
    ['O'] = 14, ['P'] = 15, ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19, ['U'] = 20,
    ['V'] = 21, ['W'] = 22, ['X'] = 23, ['Y'] = 24, ['Z'] = 25,

    ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31, ['g'] = 32,
    ['h'] = 33, ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39,
    ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43, ['s'] = 44, ['t'] = 45, ['u'] = 46,
    ['v'] = 47, ['w'] = 48, ['x'] = 49, ['y'] = 50, ['z'] = 51,

    ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55, ['4'] = 56, ['5'] = 57,
    ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61,

    ['+'] = 62, ['/'] = 63,
};

static const char lookup[64] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    '+', '/',
};

static inline bool
is_valid(unsigned char c)
{
    return reverse_lookup[c] != 0 || c == 'A' || c == '=';
}

char *
base64_decode(const char *s)
{
    const size_t len = strlen(s);

    if (len % 4 != 0)
        return NULL;

    char *ret = malloc(len / 4 * 3 + 1);

    for (size_t i = 0, o = 0; i < len; i += 4, o += 3) {
        unsigned char c0 = s[i + 0];
        unsigned char c1 = s[i + 1];
        unsigned char c2 = s[i + 2];
        unsigned char c3 = s[i + 3];

        if (!is_valid(c0) || !is_valid(c1) || !is_valid(c2) || !is_valid(c3)) {
            free(ret);
            return NULL;
        }

        unsigned a = reverse_lookup[c0];
        unsigned b = reverse_lookup[c1];
        unsigned c = reverse_lookup[c2];
        unsigned d = reverse_lookup[c3];

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
}

char *
base64_encode(const uint8_t *data, size_t size)
{
    assert(size % 3 == 0);
    if (size %3 != 0)
        return NULL;

    char *ret = malloc(size / 3 * 4 + 1);

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

        ret[o + 0] = c0;
        ret[o + 1] = c1;
        ret[o + 2] = c2;
        ret[o + 3] = c3;

        LOG_DBG("base64: encode: %c%c%c%c", c0, c1, c2, c3);
    }

    ret[size / 3 * 4] = '\0';
    return ret;
}

void
base64_encode_final(const uint8_t *data, size_t size, char result[4])
{
    assert(size > 0);
    assert(size < 3);

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
