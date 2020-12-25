#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool uri_parse(const char *uri, size_t len,
               char **scheme, char **user, char **password, char **host,
               uint16_t *port, char **path, char **query, char **fragment);

bool hostname_is_localhost(const char *hostname);
