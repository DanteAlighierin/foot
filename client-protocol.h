#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct client_string {
    uint16_t len;
    /* char str[static len]; */
};

struct client_data {
    bool hold:1;
    bool no_wait:1;
    uint8_t reserved:6;

    uint16_t cwd_len;
    uint16_t override_count;
    uint16_t argc;

    /* char cwd[static cwd_len]; */
    /* struct client_string overrides[static override_count]; */
    /* struct client_string argv[static argc]; */
} __attribute__((packed));

_Static_assert(sizeof(struct client_data) == 7, "protocol struct size error");
