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
    rbnode_t *cur = t->root;
    rbnode_t *parent = t->nil;
    int cmp = 0;

    /* invariant: cur is the subtree still to search; parent trails one node
     * behind as the eventual new node's parent (or the overwrite target)
     * once cur reaches nil or an exact match. */
    while (cur != t->nil) {
        cmp = strcmp(key, cur->key);
        if (cmp == 0) {
            if (t->value_free)
                t->value_free(cur->value);
            cur->value = value;
            return 0;
        }
        parent = cur;
        cur = (cmp < 0) ? cur->left : cur->right;
    }

    rbnode_t *n = rb_malloc(sizeof *n);
    if (!n)
        return -1;                   /* nothing allocated yet, nothing to free */

    size_t klen = strlen(key) + 1;
    char *kcopy = rb_malloc(klen);
    if (!kcopy)
        goto cleanup_node;           /* second alloc failed, unwind the first */
    memcpy(kcopy, key, klen);

    n->key    = kcopy;
    n->value  = value;
    n->left   = n->right = t->nil;
    n->parent = parent;
    n->color  = RB_RED;
    /* TODO: red-black fixup. A fresh non-root node inserted red can now
     * have a red parent, which rb_validate will reject until fixup lands. */

    if (parent == t->nil)
        t->root = n;
    else if (cmp < 0)
        parent->left = n;
    else
        parent->right = n;

    t->size++;
    return 0;

cleanup_node:
    rb_free(n);
    return -1;
}

size_t rb_size(const rbtree_t *t)
{
    return t->size;
}

void *rb_find(const rbtree_t *t, const char *key)
{
    rbnode_t *cur = t->root;

    /* invariant: cur is the subtree still to search; strcmp narrows left or
     * right each step until an exact match or the nil sentinel is reached. */
    while (cur != t->nil) {
        int cmp = strcmp(key, cur->key);
        if (cmp == 0)
            return cur->value;
        cur = (cmp < 0) ? cur->left : cur->right;
    }
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

/* Checks order, no-red-red, and black-height for the subtree rooted at x;
 * reports 1/0 and writes that subtree's black-height to *bh. lo/hi are the
 * open key bound threaded down from ancestors (NULL = unbounded), which is
 * what catches an order violation against a distant ancestor, not just x's
 * immediate parent. */
static int rb_validate_node(const rbtree_t *t, const rbnode_t *x,
                             const char *lo, const char *hi, int *bh)
{
    if (x == t->nil) {
        *bh = 0;
        return 1;
    }

    if (lo && strcmp(x->key, lo) <= 0)
        return 0;
    if (hi && strcmp(x->key, hi) >= 0)
        return 0;

    if (x->color == RB_RED &&
        (x->left->color == RB_RED || x->right->color == RB_RED))
        return 0;

    int bhL, bhR;
    if (!rb_validate_node(t, x->left, lo, x->key, &bhL))
        return 0;
    if (!rb_validate_node(t, x->right, x->key, hi, &bhR))
        return 0;
    if (bhL != bhR)
        return 0;

    *bh = bhL + (x->color == RB_BLACK ? 1 : 0);
    return 1;
}

int rb_validate(const rbtree_t *t)
{
    if (t->root->color != RB_BLACK)
        return -1;

    int bh;
    return rb_validate_node(t, t->root, NULL, NULL, &bh) ? 0 : -1;
}

/* Post-order: free a node's children before the node itself, so no pointer
 * into freed memory is ever dereferenced. Stops at t->nil, the shared
 * sentinel, which rb_destroy frees separately, once, after this returns. */
static void rb_destroy_node(rbtree_t *t, rbnode_t *n)
{
    if (n == t->nil)
        return;
    rb_destroy_node(t, n->left);
    rb_destroy_node(t, n->right);
    if (t->value_free)
        t->value_free(n->value);
    rb_free(n->key);
    rb_free(n);
}

void rb_destroy(rbtree_t *t)
{
    if (!t)
        return;
    rb_destroy_node(t, t->root);
    rb_free(t->nil);
    rb_free(t);
}
