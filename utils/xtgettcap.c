#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <ctype.h>
#include <assert.h>

static struct termios orig_termios;

static void
disable_raw_mode(void)
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) < 0)
        exit(__LINE__);
}

static void
enable_raw_mode(void)
{
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0)
        exit(__LINE__);

    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
        exit(__LINE__);
}

static const char *
hexlify(const char *s)
{
    static char buf[1024];

    const size_t len = strlen(s);
    for (size_t i = 0; i < len; i++)
        sprintf(&buf[i * 2], "%02x", s[i]);
    buf[len * 2 + 1] = '\0';

    return buf;
}

static size_t
unhexlify(char *dst, const char *src)
{
    size_t count = 0;
    for (const char *p = src; *p != '\0'; p += 2, dst++, count++)
        sscanf(p, "%02hhx", (unsigned char *)dst);

    *dst = '\0';
    return count;
}

int
main(int argc, const char *const *argv)
{
    const size_t query_count = argc - 1;

    if (query_count == 0)
        return 0;

    enable_raw_mode();

    printf("\x1bP+q");
    for (int i = 1; i < argc; i++)
        printf("%s%s", i > 1 ? ";" : "", hexlify(argv[i]));
    printf("\033\\");

    fflush(NULL);

    size_t replies = 0;
    while (replies < query_count) {
        struct pollfd fds[] = {{.fd = STDIN_FILENO, .events = POLLIN}};
        int r = poll(fds, sizeof(fds) / sizeof(fds[0]), -1);
        if (r < 0)
            exit(__LINE__);

        char buf[1024] = {0};
        ssize_t count = read(STDIN_FILENO, buf, sizeof(buf));

        if (count < 0)
            exit(__LINE__);

        if (count == 1 && buf[0] == 'q')
            break;

        printf("reply: (%zd chars): ", count);

        for (size_t i = 0; i < (size_t)count; i++) {
            if (isprint(buf[i]))
                printf("%c", buf[i]);
            else if (buf[i] == '\033')
                printf("\033[1;31m<ESC>\033[m");
            else
                printf("%02x", (uint8_t)buf[i]);
        }
        printf("\r\n");

        const char *p = buf;
        const char *end = buf + count;

        while (p < end) {

            const char *ST = strstr(p, "\033\\");
            if (ST == NULL)
                break;

            if (count < 5 ||
                (strncmp(p, "\033P1+r", 5) != 00 &&
                 strncmp(p, "\033P0+r", 5) != 0))
            {
                break;
            }

            const bool success = p[2] == '1';

            char decoded[1024];
            char copy[ST - &p[5] + 1];
            strncpy(copy, &p[5], ST - &p[5]);
            copy[ST - &p[5]] = '\0';

            char *saveptr = NULL;
            for (char *key_value = strtok_r(copy, "; ", &saveptr);
                 key_value != NULL;
                 key_value = strtok_r(NULL, "; ", &saveptr))
            {
                // printf("key-value=%s\n", key_value);
                const char *key = strtok(key_value, "=");
                const char *value = strtok(NULL, "=");

                if (key == NULL)
                    continue;

#if 0
                assert((success && value != NULL) ||
                       (!success && value == NULL));
#endif

                //printf("key=%s, value=%s\n", key, value);
                size_t len = unhexlify(decoded, key);

                if (value != NULL) {
                    decoded[len++] = '=';
                    len += unhexlify(&decoded[len], value);
                }

                const int color = success ? 39 : 31;

                printf("  \033[%dm", color);
                for (size_t i = 0 ; i < len; i++) {
                    if (isprint(decoded[i])) {
                        /* All printable characters */
                        printf("%c", decoded[i]);
                    }

                    else if (decoded[i] == '\033') {
                        /* ESC */
                        printf("\033[1;31m<ESC>\033[22;%dm", color);
                    }

                    else if (decoded[i] >= '\x00' && decoded[i] <= '\x5f') {
                        /* Control characters, e.g. ^G etc */
                        printf("\033[1m^%c\033[22m", decoded[i] + '@');
                    }

                    else if (decoded[i] == '\x7f') {
                        /* Control character ^? */
                        printf("\033[1m^?\033[22m");
                    }

                    else {
                        /* Unknown: print hex representation */
                        printf("\033[1m%02x\033[22m", (uint8_t)decoded[i]);
                    }
                }
                printf("\033[m\r\n");
                replies++;
            }

            p = ST + 2;
        }

    }

    return 0;
}
