#include "char32.h"

#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include <wctype.h>
#include <wchar.h>

#if defined __has_include
 #if __has_include (<stdc-predef.h>)
   #include <stdc-predef.h>
 #endif
#endif

#define LOG_MODULE "char32"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "macros.h"
#include "xmalloc.h"

/*
 * For now, assume we can map directly to the corresponding wchar_t
 * functions. This is true if:
 *
 *  - both data types have the same size
 *  - both use the same encoding (though we require that encoding to be UTF-32)
 */

_Static_assert(
    sizeof(wchar_t) == sizeof(char32_t), "wchar_t vs. char32_t size mismatch");

#if !defined(__STDC_UTF_32__) || !__STDC_UTF_32__
 #error "char32_t does not use UTF-32"
#endif
#if (!defined(__STDC_ISO_10646__) || !__STDC_ISO_10646__) && !defined(__FreeBSD__)
 #error "wchar_t does not use UTF-32"
#endif

UNITTEST
{
    xassert(c32len(U"") == 0);
    xassert(c32len(U"foobar") == 6);
}

UNITTEST
{
    xassert(c32cmp(U"foobar", U"foobar") == 0);
    xassert(c32cmp(U"foo", U"foobar") < 0);
    xassert(c32cmp(U"foobar", U"foo") > 0);
    xassert(c32cmp(U"a", U"b") < 0);
    xassert(c32cmp(U"b", U"a") > 0);
}

UNITTEST
{
    char32_t copy[16];
    char32_t *ret = c32ncpy(copy, U"foobar", 16);

    xassert(ret == copy);
    xassert(copy[0] == U'f');
    xassert(copy[1] == U'o');
    xassert(copy[2] == U'o');
    xassert(copy[3] == U'b');
    xassert(copy[4] == U'a');
    xassert(copy[5] == U'r');

    unsigned char zeroes[(16 - 6) * sizeof(copy[0])] = {0};
    xassert(memcmp(&copy[6], zeroes, sizeof(zeroes)) == 0);
}

UNITTEST
{
    char32_t copy[16];
    memset(copy, 0x55, sizeof(copy));

    char32_t *ret = c32cpy(copy, U"foobar");

    xassert(ret == copy);
    xassert(copy[0] == U'f');
    xassert(copy[1] == U'o');
    xassert(copy[2] == U'o');
    xassert(copy[3] == U'b');
    xassert(copy[4] == U'a');
    xassert(copy[5] == U'r');
    xassert(copy[6] == U'\0');

    unsigned char fives[(16 - 6 - 1) * sizeof(copy[0])];
    memset(fives, 0x55, sizeof(fives));
    xassert(memcmp(&copy[7], fives, sizeof(fives)) == 0);
}

UNITTEST
{
    xassert(c32casecmp(U"foobar", U"FOOBAR") == 0);
    xassert(c32casecmp(U"foo", U"FOOO") < 0);
    xassert(c32casecmp(U"FOOO", U"foo") > 0);
    xassert(c32casecmp(U"a", U"B") < 0);
    xassert(c32casecmp(U"B", U"a") > 0);
}

UNITTEST
{
    xassert(c32ncasecmp(U"foo", U"FOObar", 3) == 0);
    xassert(c32ncasecmp(U"foo", U"FOOO", 4) < 0);
    xassert(c32ncasecmp(U"FOOO", U"foo", 4) > 0);
    xassert(c32ncasecmp(U"a", U"BB", 1) < 0);
    xassert(c32ncasecmp(U"BB", U"a", 1) > 0);
}

UNITTEST
{
    char32_t dst[32] = U"foobar";
    char32_t *ret = c32ncat(dst, U"12345678XXXXXXXXX", 8);

    xassert(ret == dst);
    xassert(c32cmp(dst, U"foobar12345678") == 0);
}

UNITTEST
{
    char32_t dst[32] = U"foobar";
    char32_t *ret = c32cat(dst, U"12345678");

    xassert(ret == dst);
    xassert(c32cmp(dst, U"foobar12345678") == 0);
}

UNITTEST
{
    char32_t *c = c32dup(U"foobar");
    xassert(c32cmp(c, U"foobar") == 0);
    free(c);

    c = c32dup(U"");
    xassert(c32cmp(c, U"") == 0);
    free(c);
}

size_t
mbsntoc32(char32_t *dst, const char *src, size_t nms, size_t len)
{
    mbstate_t ps = {0};

    char32_t *out = dst;
    const char *in = src;

    size_t consumed = 0;
    size_t chars = 0;
    size_t rc;

    while ((out == NULL || chars < len) &&
           consumed < nms &&
           (rc = mbrtoc32(out, in, nms - consumed, &ps)) != 0)
    {
        switch (rc) {
        case 0:
            goto done;

        case (size_t)-1:
        case (size_t)-2:
        case (size_t)-3:
            goto err;
        }

        in += rc;
        consumed += rc;
        chars++;

        if (out != NULL)
            out++;
    }

done:
    return chars;

err:
    return (char32_t)-1;
}

UNITTEST
{
    const char input[] = "foobarzoo";
    char32_t c32[32];

    size_t ret = mbsntoc32(NULL, input, sizeof(input), 0);
    xassert(ret == 9);

    memset(c32, 0x55, sizeof(c32));
    ret = mbsntoc32(c32, input, sizeof(input), 32);

    xassert(ret == 9);
    xassert(c32[0] == U'f');
    xassert(c32[1] == U'o');
    xassert(c32[2] == U'o');
    xassert(c32[3] == U'b');
    xassert(c32[4] == U'a');
    xassert(c32[5] == U'r');
    xassert(c32[6] == U'z');
    xassert(c32[7] == U'o');
    xassert(c32[8] == U'o');
    xassert(c32[9] == U'\0');
    xassert(c32[10] == 0x55555555);

    memset(c32, 0x55, sizeof(c32));
    ret = mbsntoc32(c32, input, 1, 32);

    xassert(ret == 1);
    xassert(c32[0] == U'f');
    xassert(c32[1] == 0x55555555);

    memset(c32, 0x55, sizeof(c32));
    ret = mbsntoc32(c32, input, sizeof(input), 1);

    xassert(ret == 1);
    xassert(c32[0] == U'f');
    xassert(c32[1] == 0x55555555);
}

UNITTEST
{
    const char input[] = "foobarzoo";
    char32_t c32[32];

    size_t ret = mbstoc32(NULL, input, 0);
    xassert(ret == 9);

    memset(c32, 0x55, sizeof(c32));
    ret = mbstoc32(c32, input, 32);

    xassert(ret == 9);
    xassert(c32[0] == U'f');
    xassert(c32[1] == U'o');
    xassert(c32[2] == U'o');
    xassert(c32[3] == U'b');
    xassert(c32[4] == U'a');
    xassert(c32[5] == U'r');
    xassert(c32[6] == U'z');
    xassert(c32[7] == U'o');
    xassert(c32[8] == U'o');
    xassert(c32[9] == U'\0');
    xassert(c32[10] == 0x55555555);

    memset(c32, 0x55, sizeof(c32));
    ret = mbstoc32(c32, input, 1);

    xassert(ret == 1);
    xassert(c32[0] == U'f');
    xassert(c32[1] == 0x55555555);
}


char32_t *
ambstoc32(const char *src)
{
    if (src == NULL)
        return NULL;

    const size_t src_len = strlen(src);

    char32_t *ret = xmalloc((src_len + 1) * sizeof(ret[0]));
    mbstate_t ps = {0};

    char32_t *out = ret;
    const char *in = src;
    const char *const end = src + src_len + 1;

    size_t chars = 0;
    size_t rc;

    while ((rc = mbrtoc32(out, in, end - in, &ps)) != 0) {
        switch (rc) {
        case (size_t)-1:
        case (size_t)-2:
        case (size_t)-3:
            goto err;
        }

        in += rc;
        out++;
        chars++;
    }

    *out = U'\0';

    ret = xrealloc(ret, (chars + 1) * sizeof(ret[0]));
    return ret;

err:
    free(ret);
    return NULL;
}

UNITTEST
{
    const char* locale = setlocale(LC_CTYPE, "en_US.UTF-8");
    if (!locale)
        locale = setlocale(LC_CTYPE, "C.UTF-8");
    if (!locale)
        return;

    char32_t *hello = ambstoc32(u8"hello");
    xassert(hello != NULL);
    xassert(hello[0] == U'h');
    xassert(hello[1] == U'e');
    xassert(hello[2] == U'l');
    xassert(hello[3] == U'l');
    xassert(hello[4] == U'o');
    xassert(hello[5] == U'\0');
    free(hello);

    char32_t *swedish = ambstoc32(u8"åäö");
    xassert(swedish != NULL);
    xassert(swedish[0] == U'å');
    xassert(swedish[1] == U'ä');
    xassert(swedish[2] == U'ö');
    xassert(swedish[3] == U'\0');
    free(swedish);

    char32_t *emoji = ambstoc32(u8"👨‍👩‍👧‍👦");
    xassert(emoji != NULL);
    xassert(emoji[0] == U'👨');
    xassert(emoji[1] == U'‍');
    xassert(emoji[2] == U'👩');
    xassert(emoji[3] == U'‍');
    xassert(emoji[4] == U'👧');
    xassert(emoji[5] == U'‍');
    xassert(emoji[6] == U'👦');
    xassert(emoji[7] == U'\0');
    free(emoji);

    xassert(ambstoc32(NULL) == NULL);
    xassert(setlocale(LC_CTYPE, "C") != NULL);
}

char *
ac32tombs(const char32_t *src)
{
    if (src == NULL)
        return NULL;

    const size_t src_len = c32len(src);

    size_t allocated = src_len + 1;
    char *ret = xmalloc(allocated);
    mbstate_t ps = {0};

    char *out = ret;
    const char32_t *const end = src + src_len + 1;

    size_t bytes = 0;

    char mb[MB_CUR_MAX];

    for (const char32_t *in = src; in < end; in++) {
        size_t rc = c32rtomb(mb, *in, &ps);

        switch (rc) {
        case (size_t)-1:
            goto err;
        }

        if (bytes + rc > allocated) {
            allocated *= 2;
            ret = xrealloc(ret, allocated);
            out = &ret[bytes];
        }

        for (size_t i = 0; i < rc; i++, out++)
            *out = mb[i];

        bytes += rc;
    }

    xassert(ret[bytes - 1] == '\0');
    ret = xrealloc(ret, bytes);
    return ret;

err:
    free(ret);
    return NULL;
}

UNITTEST
{
    const char* locale = setlocale(LC_CTYPE, "en_US.UTF-8");
    if (!locale)
        locale = setlocale(LC_CTYPE, "C.UTF-8");
    if (!locale)
        return;

    char *s = ac32tombs(U"foobar");
    xassert(s != NULL);
    xassert(strcmp(s, "foobar") == 0);
    free(s);

    s = ac32tombs(U"åäö");
    xassert(s != NULL);
    xassert(strcmp(s, u8"åäö") == 0);
    free(s);

    s = ac32tombs(U"👨‍👩‍👧‍👦");
    xassert(s != NULL);
    xassert(strcmp(s, u8"👨‍👩‍👧‍👦") == 0);
    free(s);

    xassert(ac32tombs(NULL) == NULL);
    xassert(setlocale(LC_CTYPE, "C") != NULL);
}
