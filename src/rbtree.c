#include "rbtree.h"
#include "rb_alloc.h"
#include <string.h>

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

static void rb_rotate_left(rbtree_t *t, rbnode_t *nodeToRotate)
{
    rbnode_t *rightChild = nodeToRotate->right;
    nodeToRotate->right = rightChild->left;
    if (rightChild->left != t->nil)
        rightChild->left->parent = nodeToRotate;

    rightChild->parent = nodeToRotate->parent;
    if (nodeToRotate->parent == t->nil)
        t->root = rightChild;
    else if (nodeToRotate == nodeToRotate->parent->left)
        nodeToRotate->parent->left = rightChild;
    else
        nodeToRotate->parent->right = rightChild;

    rightChild->left = nodeToRotate;
    nodeToRotate->parent = rightChild;
}

static void rb_rotate_right(rbtree_t *t, rbnode_t *nodeToRotate)
{
    rbnode_t *leftChild = nodeToRotate->left;
    nodeToRotate->left = leftChild->right;
    if (leftChild->right != t->nil)
        leftChild->right->parent = nodeToRotate;

    leftChild->parent = nodeToRotate->parent;
    if (nodeToRotate->parent == t->nil)
        t->root = leftChild;
    else if (nodeToRotate == nodeToRotate->parent->right)
        nodeToRotate->parent->right = leftChild;
    else
        nodeToRotate->parent->left = leftChild;

    leftChild->right = nodeToRotate;
    nodeToRotate->parent = leftChild;
}

/* Replaces the subtree rooted at oldSubtree with newSubtree in oldSubtree's
 * parent (or t->root if it was the root), and repoints newSubtree->parent.
 * newSubtree may be t->nil. */
static void rb_splice(rbtree_t *t, rbnode_t *oldSubtree, rbnode_t *newSubtree)
{
    if (oldSubtree->parent == t->nil)
        t->root = newSubtree;
    else if (oldSubtree == oldSubtree->parent->left)
        oldSubtree->parent->left = newSubtree;
    else
        oldSubtree->parent->right = newSubtree;

    newSubtree->parent = oldSubtree->parent;
}

/* Leftmost node of the subtree rooted at start: its key is the smallest in
 * that subtree, i.e. the in-order successor of whatever node start hangs
 * off of. */
static rbnode_t *rb_inorder_successor(rbtree_t *t, rbnode_t *start)
{
    rbnode_t *cur = start;
    /* invariant: cur is the smallest-so-far candidate; each step left
     * strictly shrinks the subtree still to search. */
    while (cur->left != t->nil)
        cur = cur->left;
    return cur;
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
    if (!n) return -1;                   /* nothing allocated yet, nothing to free */

    size_t klen = strlen(key) + 1;
    char *kcopy = rb_malloc(klen);
    if (!kcopy) goto cleanup_node;           /* second alloc failed, unwind the first */
    memcpy(kcopy, key, klen);

    n->key    = kcopy;
    n->value  = value;
    n->left   = n->right = t->nil;
    n->parent = parent;
    n->color  = RB_RED;

    if (parent == t->nil) {
        t->root = n;
    } else if (cmp < 0) {
        parent->left = n;
    } else {
        parent->right = n;
    }
    

    /* TODO: red-black fixup. A fresh non-root node inserted red can now
     * have a red parent, which rb_validate will reject until fixup lands. */

    rbnode_t *problemNode = n;
    while (problemNode->parent->color == RB_RED) {
        if (problemNode->parent == problemNode->parent->parent->left) {
            rbnode_t *uncle = problemNode->parent->parent->right;  
            if (uncle->color == RB_RED) {
                // Case 1
                problemNode->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                problemNode->parent->parent->color = RB_RED;
                problemNode = problemNode->parent->parent;
            } else {
                if (problemNode == problemNode->parent->right) {
                    // Case 2
                    problemNode = problemNode->parent;
                    rb_rotate_left(t, problemNode);
                }
                // Case 3
                problemNode->parent->color = RB_BLACK;
                problemNode->parent->parent->color = RB_RED;
                rb_rotate_right(t, problemNode->parent->parent);
            }
        } else {
            // (mirror image, swap left/right)
            rbnode_t *uncle = problemNode->parent->parent->left;
            if (uncle->color == RB_RED) {
                // Case 1
                problemNode->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                problemNode->parent->parent->color = RB_RED;
                problemNode = problemNode->parent->parent;
            } else {
                if (problemNode == problemNode->parent->left) {
                    // Case 2
                    problemNode = problemNode->parent;
                    rb_rotate_right(t, problemNode);
                }
                // Case 3
                problemNode->parent->color = RB_BLACK;
                problemNode->parent->parent->color = RB_RED;
                rb_rotate_left(t, problemNode->parent->parent);
            }
        }
    }
    t->root->color = RB_BLACK;
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

int rb_delete(rbtree_t *t, const char *key)
{
    rbnode_t *target = t->root;

    /* invariant: target is the subtree still to search; strcmp narrows left
     * or right each step until an exact match or the nil sentinel is
     * reached (same search as rb_find). */
    while (target != t->nil) {
        int cmp = strcmp(key, target->key);
        if (cmp == 0)
            break;
        target = (cmp < 0) ? target->left : target->right;
    }

    if (target == t->nil)
        return -1;                    /* absent; tree unchanged */

    rbnode_t *removedColorNode = target;  /* node whose original color is leaving the tree */
    int removedColor = removedColorNode->color;
    rbnode_t *fixNode;                    /* node that inherits any missing black */

    if (target->left == t->nil) {
        fixNode = target->right;
        rb_splice(t, target, target->right);
    } else if (target->right == t->nil) {
        fixNode = target->left;
        rb_splice(t, target, target->left);
    } else {
        removedColorNode = rb_inorder_successor(t, target->right);
        removedColor = removedColorNode->color;
        fixNode = removedColorNode->right;

        if (removedColorNode->parent == target) {
            fixNode->parent = removedColorNode;   /* rb_splice is skipped below, so set this by hand */
        } else {
            rb_splice(t, removedColorNode, removedColorNode->right);
            removedColorNode->right = target->right;
            removedColorNode->right->parent = removedColorNode;
        }

        rb_splice(t, target, removedColorNode);
        removedColorNode->left = target->left;
        removedColorNode->left->parent = removedColorNode;
        removedColorNode->color = target->color;   /* successor takes target's color/position */
    }

    if (removedColor == RB_BLACK) {
        /* invariant: fixNode carries an extra black that must be resolved
         * before this loop exits; each pass either absorbs it via a
         * terminal rotation (case 4) or pushes it one level up (case 2). */
        while (fixNode != t->root && fixNode->color == RB_BLACK) {
            if (fixNode == fixNode->parent->left) {
                rbnode_t *sibling = fixNode->parent->right;
                if (sibling->color == RB_RED) {
                    // Case 1
                    sibling->color = RB_BLACK;
                    fixNode->parent->color = RB_RED;
                    rb_rotate_left(t, fixNode->parent);
                    sibling = fixNode->parent->right;
                }
                if (sibling->left->color == RB_BLACK && sibling->right->color == RB_BLACK) {
                    // Case 2
                    if (sibling != t->nil)   /* t->nil is an immutable black constant */
                        sibling->color = RB_RED;
                    fixNode = fixNode->parent;
                } else {
                    if (sibling->right->color == RB_BLACK) {
                        // Case 3
                        sibling->left->color = RB_BLACK;
                        sibling->color = RB_RED;
                        rb_rotate_right(t, sibling);
                        sibling = fixNode->parent->right;
                    }
                    // Case 4
                    sibling->color = fixNode->parent->color;
                    fixNode->parent->color = RB_BLACK;
                    sibling->right->color = RB_BLACK;
                    rb_rotate_left(t, fixNode->parent);
                    fixNode = t->root;
                }
            } else {
                // (mirror image, swap left/right)
                rbnode_t *sibling = fixNode->parent->left;
                if (sibling->color == RB_RED) {
                    // Case 1
                    sibling->color = RB_BLACK;
                    fixNode->parent->color = RB_RED;
                    rb_rotate_right(t, fixNode->parent);
                    sibling = fixNode->parent->left;
                }
                if (sibling->right->color == RB_BLACK && sibling->left->color == RB_BLACK) {
                    // Case 2
                    if (sibling != t->nil)   /* t->nil is an immutable black constant */
                        sibling->color = RB_RED;
                    fixNode = fixNode->parent;
                } else {
                    if (sibling->left->color == RB_BLACK) {
                        // Case 3
                        sibling->right->color = RB_BLACK;
                        sibling->color = RB_RED;
                        rb_rotate_left(t, sibling);
                        sibling = fixNode->parent->left;
                    }
                    // Case 4
                    sibling->color = fixNode->parent->color;
                    fixNode->parent->color = RB_BLACK;
                    sibling->left->color = RB_BLACK;
                    rb_rotate_right(t, fixNode->parent);
                    fixNode = t->root;
                }
            }
        }
        fixNode->color = RB_BLACK;
    }

    if (t->value_free)
        t->value_free(target->value);
    rb_free(target->key);
    rb_free(target);
    t->size--;
    return 0;
}

static void rb_foreach_helper(const rbtree_t *t, rbnode_t *checkNode, void (*callerFunction)(const char *key, void *value, void *callerFunctionContext), void (*callerFunctionContext)) {
    if (checkNode == t-> nil) return; // Terminate at t->nil
    rb_foreach_helper(t, checkNode->left, callerFunction, callerFunctionContext); // Recurse left first (inorder)
    callerFunction(checkNode->key, checkNode->value, callerFunctionContext); // Run callerFunction to give them their output
    rb_foreach_helper(t, checkNode->right, callerFunction, callerFunctionContext); // Recurse right last (no more left descendants in subtree)
}

void rb_foreach(const rbtree_t *t,
                void (*callerFunction)(const char *key, void *value, void *callerFunctionContext),
                void *callerFunctionContext)
{
    /* TODO: in-order traversal, stopping at t->nil. */
    rb_foreach_helper(t, t->root, callerFunction, callerFunctionContext);
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