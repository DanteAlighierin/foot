#pragma once

#include <stdint.h>
#include <stddef.h>

char *base64_decode(const char *s, size_t *out_len);
char *base64_encode(const uint8_t *data, size_t size);
void base64_encode_final(const uint8_t *data, size_t size, char result[4]);
