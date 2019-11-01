#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <linux/un.h>

#define LOG_MODULE "foot-client"
#include "log.h"
#include "version.h"

static volatile sig_atomic_t aborted = 0;

static void
sig_handler(int signo)
{
    aborted = 1;
}

static void
print_usage(const char *prog_name)
{
    printf("Usage: %s [OPTIONS]...\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -v,--version                show the version number and quit\n");
}

int
main(int argc, char *const *argv)
{
    int ret = EXIT_FAILURE;

    const char *const prog_name = argv[0];

    while (true) {
        int c = getopt_long(argc, argv, ":hv", NULL, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 'v':
            printf("footclient version %s\n", FOOT_VERSION);
            return EXIT_SUCCESS;

        case 'h':
            print_usage(prog_name);
            return EXIT_SUCCESS;

       case ':':
            fprintf(stderr, "error: -%c: missing required argument\n", optopt);
            return EXIT_FAILURE;

        case '?':
            fprintf(stderr, "error: -%c: invalid option\n", optopt);
            return EXIT_FAILURE;
        }
    }

    argc -= optind;
    argv += optind;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        LOG_ERRNO("failed to create socket");
        goto err;
    }

    bool connected = false;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};

    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime != NULL) {
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/foot.sock", xdg_runtime);

        if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0)
            connected = true;
    }

    if (!connected) {
        strncpy(addr.sun_path, "/tmp/foot.sock", sizeof(addr.sun_path) - 1);
        if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
            LOG_ERRNO("failed to connect (is 'foot --server' running?)");
            goto err;
        }
    }

    if (send(fd, &argc, sizeof(argc), 0) != sizeof(argc)) {
        LOG_ERRNO("failed to send argc/argv to server");
        goto err;
    }

    for (int i = 0; i < argc; i++) {
        uint16_t len = strlen(argv[i]);

        if (send(fd, &len, sizeof(len), 0) != sizeof(len) ||
            send(fd, argv[i], len, 0) != sizeof(len))
        {
            LOG_ERRNO("failed to send argc/argv to server");
            goto err;
        }
    }

    if (sigaction(SIGINT, &(struct sigaction){.sa_handler = &sig_handler}, NULL) < 0) {
        LOG_ERRNO("failed to register signal handlers");
        goto err;
    }

    int exit_code;
    ssize_t rcvd = recv(fd, &exit_code, sizeof(exit_code), 0);

    if (rcvd == -1 && errno == EINTR)
        assert(aborted);
    else if (rcvd != sizeof(exit_code))
        LOG_ERRNO("failed to read server response");
    else
        ret = exit_code;

err:
    if (fd != -1)
        close(fd);
    return ret;
}
