#pragma once

#include <stdint.h>
#include <wchar.h>

struct composed {
    wchar_t *chars;
    struct composed *left;
    struct composed *right;
    uint32_t key;
    uint8_t count;
    uint8_t width;
};

struct composed *composed_lookup(struct composed *root, uint32_t key);
uint32_t composed_insert(struct composed **root, struct composed *node);

void composed_free(struct composed *root);
