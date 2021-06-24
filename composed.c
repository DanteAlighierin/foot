#include "composed.h"

#include <stdlib.h>
#include <stdbool.h>

#include "debug.h"

struct composed *
composed_lookup(struct composed *root, uint32_t key)
{
    struct composed *node = root;

    while (node != NULL) {
        if (key == node->key)
            return node;

        node = key < node->key ? node->left : node->right;
    }

    return NULL;
}

uint32_t
composed_insert(struct composed **root, struct composed *node)
{
    node->left = node->right = NULL;

    if (*root == NULL) {
        *root = node;
        return node->key;
    }

    uint32_t key = node->key;

    struct composed *prev = NULL;
    struct composed *n = *root;

    while (n != NULL) {
        if (n->key == node->key) {
            /* TODO: wrap around at (CELL_COMB_CHARS_HI - CELL_COMB_CHARS_LO) */
            key++;
        }

        prev = n;
        n = key < n->key ? n->left : n->right;
    }

    xassert(prev != NULL);
    xassert(n == NULL);

    /* May have been changed */
    node->key = key;

    if (key < prev->key) {
        xassert(prev->left == NULL);
        prev->left = node;
    } else {
        xassert(prev->right == NULL);
        prev->right = node;
    }

    return key;
}

void
composed_free(struct composed *root)
{
    if (root == NULL)
        return;

    composed_free(root->left);
    composed_free(root->right);

    free(root->chars);
    free(root);
}
