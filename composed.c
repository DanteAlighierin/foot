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

void
composed_insert(struct composed **root, struct composed *node)
{
    node->left = node->right = NULL;

    if (*root == NULL) {
        *root = node;
        return;
    }

    uint32_t key = node->key;

    struct composed *prev = NULL;
    struct composed *n = *root;

    while (n != NULL) {
        xassert(n->key != node->key);

        prev = n;
        n = key < n->key ? n->left : n->right;
    }

    xassert(prev != NULL);
    xassert(n == NULL);

    if (key < prev->key) {
        xassert(prev->left == NULL);
        prev->left = node;
    } else {
        xassert(prev->right == NULL);
        prev->right = node;
    }
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
