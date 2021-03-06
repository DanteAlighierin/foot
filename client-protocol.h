#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct client_argv {
    uint16_t len;
    /* char arg[static len]; */
};

struct client_data {
    unsigned width;
    unsigned height;
    uint8_t size_type:1; // Values correspond to enum conf_size_type
    uint8_t maximized:1;
    uint8_t fullscreen:1;
    uint8_t hold:1;
    uint8_t login_shell:1;
    uint8_t no_wait:1;

    uint16_t cwd_len;
    uint16_t term_len;
    uint16_t title_len;
    uint16_t app_id_len;

    uint16_t argc;

    /* char cwd[static cwd_len]; */
    /* char term[static term_len]; */
    /* char title[static title_len]; */
    /* char app_id[static app_id_len]; */

    /* struct client_argv argv[static argc]; */
};
