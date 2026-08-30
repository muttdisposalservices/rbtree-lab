#include "rbtree.h"
#include <stdlib.h>
#include <string.h>

/* Sole allocation path (CLAUDE.md routing rule). Weak so a test build (or a
 * future assignment) can link in a strong override for a custom allocator. */
__attribute__((weak)) void *rb_malloc(size_t size)
{
    return malloc(size);
}

__attribute__((weak)) void rb_free(void *ptr)
{
    free(ptr);
}

typedef struct rbnode {
    char *key;               /* owned copy; NULL on the nil sentinel */
    void *value;              /* owned iff value_free != NULL */
    struct rbnode *left;
    struct rbnode *right;
    struct rbnode *parent;
    enum { RB_RED, RB_BLACK } color;
} rbnode_t;

struct rbtree {
    rbnode_t *root;           /* == nil when empty */
    rbnode_t *nil;            /* shared black sentinel leaf; not counted in size */
    size_t size;
    rb_value_free_fn value_free;
};

rbtree_t *rb_create(rb_value_free_fn value_free)
{
    rbtree_t *t = rb_malloc(sizeof *t);
    if (!t)
        return NULL;                 /* nothing allocated yet, nothing to free */

    rbnode_t *nil = rb_malloc(sizeof *nil);
    if (!nil)
        goto cleanup_tree;           /* second alloc failed, unwind the first */

    nil->color  = RB_BLACK;
    nil->key    = NULL;
    nil->value  = NULL;
    nil->left = nil->right = nil->parent = nil;

    t->nil        = nil;
    t->root        = nil;
    t->size        = 0;
    t->value_free = value_free;
    return t;

cleanup_tree:
    rb_free(t);
    return NULL;
}

int rb_insert(rbtree_t *t, const char *key, void *value)
{
    (void)t;
    (void)key;
    (void)value;
    /* TODO: alloc node (goto-cleanup on either failure), BST insert against
     * nil leaves, red-black fixup, size++.
     * NOTE: copy the key via rb_malloc+memcpy, not strdup -- strdup calls
     * malloc directly, bypassing the CLAUDE.md allocation-routing rule. */
    return -1;
}

size_t rb_size(const rbtree_t *t)
{
    return t->size;
}

void *rb_find(const rbtree_t *t, const char *key)
{
    (void)t;
    (void)key;
    /* TODO: strcmp walk from root until node == t->nil. */
    return NULL;
}

void rb_foreach(const rbtree_t *t,
                void (*fn)(const char *key, void *value, void *ctx),
                void *ctx)
{
    (void)t;
    (void)fn;
    (void)ctx;
    /* TODO: in-order traversal, stopping at t->nil. */
}

int rb_validate(const rbtree_t *t)
{
    (void)t;
    /* TODO: BST order, red implies black children, equal black-height on
     * every root-to-nil path. */
    return -1;
}

void rb_destroy(rbtree_t *t)
{
    if (!t)
        return;
    /* TODO: once rb_insert lands real nodes, traverse and free each node's
     * key (and value, iff value_free is set) before freeing nil/t. Today
     * rb_create's only reachable output is an empty tree, so there are no
     * nodes/keys/values to walk yet. */
    rb_free(t->nil);
    rb_free(t);
}
