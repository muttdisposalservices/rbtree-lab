#include "rbtree.h"
#include <stdlib.h>
#include <string.h>

/* Sole allocation path (CLAUDE.md routing rule). */
static void *rb_malloc(size_t size)
{
    return malloc(size);
}

static void rb_free(void *ptr)
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

/* TODO: rb_destroy is undeclared here but will likely be needed before
 * `make memcheck` can stay green once rb_create/rb_insert do real
 * allocation -- there's currently no way to free a tree's nodes/keys. */
