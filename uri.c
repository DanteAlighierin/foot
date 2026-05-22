#include "uri.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#define LOG_MODULE "uri"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "util.h"
#include "xmalloc.h"

bool
uri_parse(const char *uri, size_t len,
          char **scheme, char **user, char **password, char **host,
          uint16_t *port, char **path, char **query, char **fragment)
{
    LOG_DBG("parse URI: \"%.*s\"", (int)len, uri);

    if (scheme != NULL)   *scheme = NULL;
    if (user != NULL)     *user = NULL;
    if (password != NULL) *password = NULL;
    if (host != NULL)     *host = NULL;
    if (port != NULL)     *port = 0;
    if (path != NULL)     *path = NULL;
    if (query != NULL)    *query = NULL;
    if (fragment != NULL) *fragment = NULL;

    size_t left = len;
    const char *start = uri;
    const char *end = NULL;

    if ((end = memchr(start, ':', left)) == NULL)
        goto err;

    size_t scheme_len = end - start;
    if (scheme_len == 0)
        goto err;

    if (scheme != NULL)
        *scheme = xstrndup(start, scheme_len);

    LOG_DBG("scheme: \"%.*s\"", (int)scheme_len, start);

    start = end + 1;
    left = len - (start - uri);

    /* Authinfo */
    if (left >= 2 && start[0] == '/' && start[1] == '/') {
        start += 2;
        left -= 2;

        /* [user[:password]@]@host[:port] */

        /* Find beginning of path segment (required component
         * following the authinfo) */
        const char *path_segment = memchr(start, '/', left);
        if (path_segment == NULL)
            goto err;

        size_t auth_left = path_segment - start;

        /* Do we have a user (and optionally a password)? */
        const char *user_pw_end = memchr(start, '@', auth_left);
        if (user_pw_end != NULL) {
            size_t user_pw_len = user_pw_end - start;

            /* Do we have a password? */
            const char *user_end = memchr(start, ':', user_pw_end - start);
            if (user_end != NULL) {
                size_t user_len = user_end - start;
                if (user_len == 0)
                    goto err;

                if (user != NULL)
                    *user = xstrndup(start, user_len);

                const char *pw = user_end + 1;
                size_t pw_len = user_pw_end - pw;
                if (pw_len == 0)
                    goto err;

                if (password != NULL)
                    *password = xstrndup(pw, pw_len);

                LOG_DBG("user: \"%.*s\"", (int)user_len, start);
                LOG_DBG("password: \"%.*s\"", (int)pw_len, pw);
            } else {
                size_t user_len = user_pw_end - start;
                if (user_len == 0)
                    goto err;

                if (user != NULL)
                    *user = xstrndup(start, user_len);

                LOG_DBG("user: \"%.*s\"", (int)user_len, start);
            }

            start = user_pw_end + 1;
            left = len - (start - uri);
            auth_left -= user_pw_len + 1;
        }

        const char *host_end = memchr(start, ':', auth_left);
        if (host_end != NULL) {
            size_t host_len = host_end - start;
            if (host != NULL)
                *host = xstrndup(start, host_len);

            const char *port_str = host_end + 1;
            size_t port_len = path_segment - port_str;
            if (port_len == 0)
                goto err;

            uint16_t _port = 0;
            for (size_t i = 0; i < port_len; i++) {
                if (!(port_str[i] >= '0' && port_str[i] <= '9'))
                    goto err;

                _port *= 10;
                _port += port_str[i] - '0';
            }

            if (port != NULL)
                *port = _port;

            LOG_DBG("host: \"%.*s\"", (int)host_len, start);
            LOG_DBG("port: \"%.*s\" (%hu)", (int)port_len, port_str, _port);
        } else {
            size_t host_len = path_segment - start;
            if (host != NULL)
                *host = xstrndup(start, host_len);

            LOG_DBG("host: \"%.*s\"", (int)host_len, start);
        }

        start = path_segment;
        left = len - (start - uri);
    }

    /* Do we have a query? */
    const char *query_start = memchr(start, '?', left);
    const char *fragment_start = memchr(start, '#', left);

    if (scheme != NULL && streq(*scheme, "file")) {
        /* Don't try to parse query/fragment in file URIs, just treat
           the remaining text as path */
        query_start = NULL;
        fragment_start = NULL;
    }

    else if (query_start != NULL && fragment_start != NULL &&
        fragment_start < query_start)
    {
        /* Invalid URI - for now, ignore, and treat is as part of path */
        query_start = NULL;
        fragment_start = NULL;
    }

    size_t path_len =
        query_start != NULL ? query_start - start :
        fragment_start != NULL ? fragment_start - start :
        left;

    if (path_len == 0)
        goto err;

    /* Path - decode %xx encoded characters */
    if (path != NULL) {
        const char *encoded = start;
        char *decoded = xmalloc(path_len + 1);
        char *p = decoded;

        size_t encoded_len = path_len;
        size_t UNUSED decoded_len = 0;

        while (true) {
            /* Find next '%' */
            const char *next = memchr(encoded, '%', encoded_len);

            if (next == NULL) {
                strncpy(p, encoded, encoded_len);
                decoded_len += encoded_len;
                p += encoded_len;
                break;
            }

            /* Copy everything leading up to the '%' */
            size_t prefix_len = next - encoded;
            memcpy(p, encoded, prefix_len);

            p += prefix_len;
            encoded_len -= prefix_len;
            decoded_len += prefix_len;

            if (encoded_len >= 3 &&
                hex2nibble(next[1]) <= 15 && hex2nibble(next[2]) <= 15)
            {
                *p++ = hex2nibble(next[1]) << 4 | hex2nibble(next[2]);
                decoded_len++;
                encoded_len -= 3;
                encoded = next + 3;
            } else {
                *p++ = *next;
                decoded_len++;
                encoded_len -= 1;
                encoded = next + 1;
            }
        }

        *p = '\0';
        *path = decoded;

        LOG_DBG("path: encoded=\"%.*s\", decoded=\"%s\"", (int)path_len, start, decoded);
    } else
        LOG_DBG("path: encoded=\"%.*s\", decoded=<skipped>", (int)path_len, start);

    start = query_start != NULL ? query_start + 1 : fragment_start != NULL ? fragment_start + 1 : uri + len;
    left = len - (start - uri);

    if (query_start != NULL) {
        size_t query_len = fragment_start != NULL
            ? fragment_start - start : left;

        if (query_len == 0)
            goto err;

        if (query != NULL)
            *query = xstrndup(start, query_len);

        LOG_DBG("query: \"%.*s\"", (int)query_len, start);

        start = fragment_start != NULL ? fragment_start + 1 : uri + len;
        left = len - (start - uri);
    }

    if (fragment_start != NULL) {
        if (left == 0)
            goto err;

        if (fragment != NULL)
            *fragment = xstrndup(start, left);

        LOG_DBG("fragment: \"%.*s\"", (int)left, start);
    }

    return true;

err:
    if (scheme != NULL)   free(*scheme);
    if (user != NULL)     free(*user);
    if (password != NULL) free(*password);
    if (host != NULL)     free(*host);
    if (path != NULL)     free(*path);
    if (query != NULL)    free(*query);
    if (fragment != NULL) free(*fragment);
    return false;
}

bool
hostname_is_localhost(const char *hostname)
{
    char this_host[_POSIX_HOST_NAME_MAX];
    if (gethostname(this_host, sizeof(this_host)) < 0)
        this_host[0] = '\0';

    return (hostname != NULL && (
                streq(hostname, "") ||
                streq(hostname, "localhost") ||
                streq(hostname, this_host)));
}

UNITTEST
{
    /* Valid URI, all components */
    const char uri[] = "http://john:wick@www.foobar.com:80/this/is/the%20path?query1=abc&query2=def#fragment";

    char *scheme, *user, *password, *host, *path, *query, *fragment;
    uint16_t port = 0;

    uri_parse(
        uri, sizeof(uri) - 1, &scheme, &user, &password, &host, &port, &path,
        &query, &fragment);
    xassert(streq(scheme, "http")); free(scheme);
    xassert(streq(user, "john")); free(user);
    xassert(streq(password, "wick")); free(password);
    xassert(streq(host, "www.foobar.com")); free(host);
    xassert(port == 80);
    xassert(streq(path, "/this/is/the path")); free(path);
    xassert(streq(query, "query1=abc&query2=def")); free(query);
    xassert(streq(fragment, "fragment")); free(fragment);
}

UNITTEST
{
    /* Valid URI, all components. Using file scheme, so query+fragment is treated as part of the path */
    const char uri[] = "file://john:wick@www.foobar.com:80/this/is/the%20path?query1=abc&query2=def#fragment";

    char *scheme, *user, *password, *host, *path, *query, *fragment;
    uint16_t port = 0;

    uri_parse(
        uri, sizeof(uri) - 1, &scheme, &user, &password, &host, &port, &path,
        &query, &fragment);
    xassert(streq(scheme, "file")); free(scheme);
    xassert(streq(user, "john")); free(user);
    xassert(streq(password, "wick")); free(password);
    xassert(streq(host, "www.foobar.com")); free(host);
    xassert(port == 80);
    xassert(streq(path, "/this/is/the path?query1=abc&query2=def#fragment")); free(path);
    xassert(query == NULL);
    xassert(fragment == NULL);
}

UNITTEST
{
    /* Test a valid URI containing all components, except password */
    const char uri[] = "http://john@www.foobar.com:80/this/is/the%20path?query1=abc&query2=def#fragment";

    char *scheme, *user, *password, *host, *path, *query, *fragment;
    uint16_t port = 0;

    uri_parse(
        uri, sizeof(uri) - 1, &scheme, &user, &password, &host, &port, &path,
        &query, &fragment);
    xassert(streq(scheme, "http")); free(scheme);
    xassert(streq(user, "john")); free(user);
    xassert(password == NULL);
    xassert(streq(host, "www.foobar.com")); free(host);
    xassert(port == 80);
    xassert(streq(path, "/this/is/the path")); free(path);
    xassert(streq(query, "query1=abc&query2=def")); free(query);
    xassert(streq(fragment, "fragment")); free(fragment);
}

UNITTEST
{
    /* Valid URI, all components, except user+password */
    const char uri[] = "http://www.foobar.com:80/this/is/the%20path?query1=abc&query2=def#fragment";

    char *scheme, *user, *password, *host, *path, *query, *fragment;
    uint16_t port = 0;

    uri_parse(
        uri, sizeof(uri) - 1, &scheme, &user, &password, &host, &port, &path,
        &query, &fragment);
    xassert(streq(scheme, "http")); free(scheme);
    xassert(user == NULL);
    xassert(password == NULL);
    xassert(streq(host, "www.foobar.com")); free(host);
    xassert(port == 80);
    xassert(streq(path, "/this/is/the path")); free(path);
    xassert(streq(query, "query1=abc&query2=def")); free(query);
    xassert(streq(fragment, "fragment")); free(fragment);
}

UNITTEST
{
    /* Valid URI, fragment, no query */
    const char uri[] = "http://www.foobar.com:80/this/is/the%20path#fragment";

    char *scheme, *user, *password, *host, *path, *query, *fragment;
    uint16_t port = 0;

    uri_parse(
        uri, sizeof(uri) - 1, &scheme, &user, &password, &host, &port, &path,
        &query, &fragment);
    xassert(streq(scheme, "http")); free(scheme);
    xassert(user == NULL);
    xassert(password == NULL);
    xassert(streq(host, "www.foobar.com")); free(host);
    xassert(port == 80);
    xassert(streq(path, "/this/is/the path")); free(path);
    xassert(query == NULL);
    xassert(streq(fragment, "fragment")); free(fragment);
}

UNITTEST
{
    /* Valid URI, query, no fragment */
    const char uri[] = "http://www.foobar.com:80/this/is/the%20path?query1=abc&query2=def";

    char *scheme, *user, *password, *host, *path, *query, *fragment;
    uint16_t port = 0;

    uri_parse(
        uri, sizeof(uri) - 1, &scheme, &user, &password, &host, &port, &path,
        &query, &fragment);
    xassert(streq(scheme, "http")); free(scheme);
    xassert(user == NULL);
    xassert(password == NULL);
    xassert(streq(host, "www.foobar.com")); free(host);
    xassert(port == 80);
    xassert(streq(path, "/this/is/the path")); free(path);
    xassert(streq(query, "query1=abc&query2=def")); free(query);
    xassert(fragment == NULL);
}

UNITTEST
{
    /* Malformed URI, fragment before query (treated as part of path instead) */
    const char uri[] = "http://www.foobar.com:80/this/is/the%20path#fragment?query1=abc&query2=def";

    char *scheme, *user, *password, *host, *path, *query, *fragment;
    uint16_t port = 0;

    uri_parse(
        uri, sizeof(uri) - 1, &scheme, &user, &password, &host, &port, &path,
        &query, &fragment);
    xassert(streq(scheme, "http")); free(scheme);
    xassert(user == NULL);
    xassert(password == NULL);
    xassert(streq(host, "www.foobar.com")); free(host);
    xassert(port == 80);
    xassert(streq(path, "/this/is/the path#fragment?query1=abc&query2=def")); free(path);
    xassert(query == NULL);
    xassert(fragment == NULL);
}

UNITTEST
{
    /* Malformed URI, trailing '%' */
    const char uri[] = "file:///%ABNOT-PART-OF-INPUT";
    char *path;
    uri_parse(uri, 9, NULL, NULL, NULL, NULL, NULL, &path, NULL, NULL);
    xassert(streq(path, "/%")); free(path);
}

UNITTEST
{
    /* Malformed URI, trailing '%2' */
    const char uri[] = "file:///%2ANOT-PART-OF-INPUT";
    char *path;
    uri_parse(uri, 10, NULL, NULL, NULL, NULL, NULL, &path, NULL, NULL);
    xassert(streq(path, "/%2")); free(path);
}

UNITTEST
{
    /* Malformed URI, trailing '%ag' */
    const char uri[] = "file:///%ag";
    char *path;
    uri_parse(uri, sizeof(uri) - 1, NULL, NULL, NULL, NULL, NULL, &path, NULL, NULL);
    xassert(streq(path, "/%ag")); free(path);
}
