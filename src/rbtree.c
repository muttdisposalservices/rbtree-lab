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
    rbtree_t *tree = rb_malloc(sizeof *tree);
    if (!tree)
        return NULL;                 /* nothing allocated yet, nothing to free */

    rbnode_t *nil = rb_malloc(sizeof *nil);
    if (!nil)
        goto cleanup_tree;           /* second alloc failed, unwind the first */

    nil->color  = RB_BLACK;
    nil->key    = NULL;
    nil->value  = NULL;
    nil->left = nil->right = nil->parent = nil;

    tree->nil        = nil;
    tree->root        = nil;
    tree->size        = 0;
    tree->value_free = value_free;
    return tree;

cleanup_tree:
    rb_free(tree);
    return NULL;
}

static void rb_rotate_left(rbtree_t *tree, rbnode_t *nodeToRotate)
{
    rbnode_t *rightChild = nodeToRotate->right;
    nodeToRotate->right = rightChild->left;
    if (rightChild->left != tree->nil)
        rightChild->left->parent = nodeToRotate;

    rightChild->parent = nodeToRotate->parent;
    if (nodeToRotate->parent == tree->nil)
        tree->root = rightChild;
    else if (nodeToRotate == nodeToRotate->parent->left)
        nodeToRotate->parent->left = rightChild;
    else
        nodeToRotate->parent->right = rightChild;

    rightChild->left = nodeToRotate;
    nodeToRotate->parent = rightChild;
}

static void rb_rotate_right(rbtree_t *tree, rbnode_t *nodeToRotate)
{
    rbnode_t *leftChild = nodeToRotate->left;
    nodeToRotate->left = leftChild->right;
    if (leftChild->right != tree->nil)
        leftChild->right->parent = nodeToRotate;

    leftChild->parent = nodeToRotate->parent;
    if (nodeToRotate->parent == tree->nil)
        tree->root = leftChild;
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
static void rb_splice(rbtree_t *tree, rbnode_t *oldSubtree, rbnode_t *newSubtree)
{
    if (oldSubtree->parent == tree->nil)
        tree->root = newSubtree;
    else if (oldSubtree == oldSubtree->parent->left)
        oldSubtree->parent->left = newSubtree;
    else
        oldSubtree->parent->right = newSubtree;

    newSubtree->parent = oldSubtree->parent;
}

/* Leftmost node of the subtree rooted at start: its key is the smallest in
 * that subtree, i.e. the in-order successor of whatever node start hangs
 * off of. */
static rbnode_t *rb_inorder_successor(rbtree_t *tree, rbnode_t *start)
{
    rbnode_t *current = start;
    /* invariant: cur is the smallest-so-far candidate; each step left
     * strictly shrinks the subtree still to search. */
    while (current->left != tree->nil)
        current = current->left;
    return current;
}

int rb_insert(rbtree_t *tree, const char *key, void *value)
{
    rbnode_t *current = tree->root;
    rbnode_t *parent = tree->nil;
    int cmp = 0;

    /* invariant: cur is the subtree still to search; parent trails one node
     * behind as the eventual new node's parent (or the overwrite target)
     * once cur reaches nil or an exact match. */
    while (current != tree->nil) {
        cmp = strcmp(key, current->key);
        if (cmp == 0) {
            if (tree->value_free)
                tree->value_free(current->value);
            current->value = value;
            return 0;
        }
        parent = current;
        current = (cmp < 0) ? current->left : current->right;
    }

    rbnode_t *newNode = rb_malloc(sizeof *newNode);
    if (!newNode) return -1;                   /* nothing allocated yet, nothing to free */

    size_t keyLength = strlen(key) + 1;
    char *keyCopy = rb_malloc(keyLength);
    if (!keyCopy) goto cleanup_node;           /* second alloc failed, unwind the first */
    memcpy(keyCopy, key, keyLength);

    newNode->key = keyCopy;
    newNode->value = value;
    newNode->left = newNode->right = tree->nil;
    newNode->parent = parent;
    newNode->color  = RB_RED;

    if (parent == tree->nil) {
        tree->root = newNode;
    } else if (cmp < 0) {
        parent->left = newNode;
    } else {
        parent->right = newNode;
    }
    

    rbnode_t *problemNode = newNode;
    while (problemNode->parent->color == RB_RED) {
        if (problemNode->parent == problemNode->parent->parent->left) {
            rbnode_t *uncle = problemNode->parent->parent->right;  
            if (uncle->color == RB_RED) {
                // Red uncle: Color parent+uncle black, grandparent red, push check up to grandparent
                problemNode->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                problemNode->parent->parent->color = RB_RED;
                problemNode = problemNode->parent->parent;
            } else {
                if (problemNode == problemNode->parent->right) {
                    // Black uncle: Triangle/zigzag case; rotate and push problem to parent
                    problemNode = problemNode->parent;
                    rb_rotate_left(tree, problemNode);
                }
                // Black uncle: Line case; color parent black, grandparent red, and rotate grandparent
                problemNode->parent->color = RB_BLACK;
                problemNode->parent->parent->color = RB_RED;
                rb_rotate_right(tree, problemNode->parent->parent);
            }
        } else {
            // (mirror image, swap left/right)
            rbnode_t *uncle = problemNode->parent->parent->left;
            if (uncle->color == RB_RED) {
                // Red uncle: Color parent+uncle black, grandparent red, push check up to grandparent
                problemNode->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                problemNode->parent->parent->color = RB_RED;
                problemNode = problemNode->parent->parent;
            } else {
                if (problemNode == problemNode->parent->left) {
                    // Black uncle: Triangle/zigzag case; rotate and push problem to parent
                    problemNode = problemNode->parent;
                    rb_rotate_right(tree, problemNode);
                }
                // Black uncle: Line case; color parent black, grandparent red, and rotate grandparent
                problemNode->parent->color = RB_BLACK;
                problemNode->parent->parent->color = RB_RED;
                rb_rotate_left(tree, problemNode->parent->parent);
            }
        }
    }
    tree->root->color = RB_BLACK;
    tree->size++;
    return 0;

cleanup_node:
    rb_free(newNode);
    return -1;
}

size_t rb_size(const rbtree_t *tree)
{
    return tree->size;
}

void *rb_find(const rbtree_t *tree, const char *key)
{
    rbnode_t *current = tree->root;

    while (current != tree->nil) {
        int cmp = strcmp(key, current->key);
        if (cmp == 0)
            return current->value;
        current = (cmp < 0) ? current->left : current->right;
    }
    return NULL;
}

int rb_delete(rbtree_t *tree, const char *key)
{
    rbnode_t *target = tree->root;

    while (target != tree->nil) {
        int cmp = strcmp(key, target->key);
        if (cmp == 0)
            break;
        target = (cmp < 0) ? target->left : target->right;
    }

    if (target == tree->nil)
        return -1;                    /* absent; tree unchanged */

    rbnode_t *removedNode = target;
    int removedColor = removedNode->color;
    rbnode_t *fixNode;                    /* node that inherits black debt from deletion */

    if (target->left == tree->nil) {
        fixNode = target->right;
        rb_splice(tree, target, target->right);
    } else if (target->right == tree->nil) {
        fixNode = target->left;
        rb_splice(tree, target, target->left);
    } else {
        removedNode = rb_inorder_successor(tree, target->right);
        removedColor = removedNode->color;
        fixNode = removedNode->right;

        if (removedNode->parent == target) {
            fixNode->parent = removedNode;   /* rb_splice is skipped below, so set this by hand */
        } else {
            rb_splice(tree, removedNode, removedNode->right);
            removedNode->right = target->right;
            removedNode->right->parent = removedNode;
        }

        rb_splice(tree, target, removedNode);
        removedNode->left = target->left;
        removedNode->left->parent = removedNode;
        removedNode->color = target->color;   /* successor takes target's color/position */
    }

    if (removedColor == RB_BLACK) {
        /* invariant: fixNode carries an extra black that must be resolved
         * before this loop exits; each pass either absorbs it via a
         * terminal rotation or pushes it one level up. */
        while (fixNode != tree->root && fixNode->color == RB_BLACK) {
            if (fixNode == fixNode->parent->left) {
                rbnode_t *sibling = fixNode->parent->right;
                if (sibling->color == RB_RED) {
                    // Case 1
                    sibling->color = RB_BLACK;
                    fixNode->parent->color = RB_RED;
                    rb_rotate_left(tree, fixNode->parent);
                    sibling = fixNode->parent->right;
                }
                if (sibling->left->color == RB_BLACK && sibling->right->color == RB_BLACK) {
                    // Case 2
                    if (sibling != tree->nil)   /* t->nil is an immutable black constant */
                        sibling->color = RB_RED;
                    fixNode = fixNode->parent;
                } else {
                    if (sibling->right->color == RB_BLACK) {
                        // Case 3
                        sibling->left->color = RB_BLACK;
                        sibling->color = RB_RED;
                        rb_rotate_right(tree, sibling);
                        sibling = fixNode->parent->right;
                    }
                    // Case 4
                    sibling->color = fixNode->parent->color;
                    fixNode->parent->color = RB_BLACK;
                    sibling->right->color = RB_BLACK;
                    rb_rotate_left(tree, fixNode->parent);
                    fixNode = tree->root;
                }
            } else {
                // (mirror image, swap left/right)
                rbnode_t *sibling = fixNode->parent->left;
                if (sibling->color == RB_RED) {
                    // Case 1
                    sibling->color = RB_BLACK;
                    fixNode->parent->color = RB_RED;
                    rb_rotate_right(tree, fixNode->parent);
                    sibling = fixNode->parent->left;
                }
                if (sibling->right->color == RB_BLACK && sibling->left->color == RB_BLACK) {
                    // Case 2
                    if (sibling != tree->nil)   /* t->nil is an immutable black constant */
                        sibling->color = RB_RED;
                    fixNode = fixNode->parent;
                } else {
                    if (sibling->left->color == RB_BLACK) {
                        // Case 3
                        sibling->right->color = RB_BLACK;
                        sibling->color = RB_RED;
                        rb_rotate_left(tree, sibling);
                        sibling = fixNode->parent->left;
                    }
                    // Case 4
                    sibling->color = fixNode->parent->color;
                    fixNode->parent->color = RB_BLACK;
                    sibling->left->color = RB_BLACK;
                    rb_rotate_right(tree, fixNode->parent);
                    fixNode = tree->root;
                }
            }
        }
        fixNode->color = RB_BLACK;
    }

    if (tree->value_free)
        tree->value_free(target->value);
    rb_free(target->key);
    rb_free(target);
    tree->size--;
    return 0;
}

static void rb_foreach_helper(const rbtree_t *tree, rbnode_t *checkNode, void (*callerFunction)(const char *key, void *value, void *callerFunctionContext), void (*callerFunctionContext)) {
    if (checkNode == tree-> nil) return; // Terminate at t->nil
    rb_foreach_helper(tree, checkNode->left, callerFunction, callerFunctionContext); // Recurse left first (inorder)
    callerFunction(checkNode->key, checkNode->value, callerFunctionContext); // Run callerFunction to give them their output
    rb_foreach_helper(tree, checkNode->right, callerFunction, callerFunctionContext); // Recurse right last (no more left descendants in subtree)
}

void rb_foreach(const rbtree_t *tree,
                void (*callerFunction)(const char *key, void *value, void *callerFunctionContext),
                void *callerFunctionContext)
{
    /* TODO: in-order traversal, stopping at t->nil. */
    rb_foreach_helper(tree, tree->root, callerFunction, callerFunctionContext);
}

/* Checks order, no-red-red, and black-height for the subtree rooted at x;
 * reports 1/0 and writes that subtree's black-height to *bh. lo/hi are the
 * open key bound threaded down from ancestors (NULL = unbounded), which is
 * what catches an order violation against a distant ancestor, not just x's
 * immediate parent. */
static int rb_validate_node(const rbtree_t *tree, const rbnode_t *current,
                             const char *lo, const char *hi, int *bh)
{
    if (current == tree->nil) {
        *bh = 0;
        return 1;
    }

    if (lo && strcmp(current->key, lo) <= 0)
        return 0;
    if (hi && strcmp(current->key, hi) >= 0)
        return 0;

    if (current->color == RB_RED &&
        (current->left->color == RB_RED || current->right->color == RB_RED))
        return 0;

    int bhL, bhR;
    if (!rb_validate_node(tree, current->left, lo, current->key, &bhL))
        return 0;
    if (!rb_validate_node(tree, current->right, current->key, hi, &bhR))
        return 0;
    if (bhL != bhR)
        return 0;

    *bh = bhL + (current->color == RB_BLACK ? 1 : 0);
    return 1;
}

int rb_validate(const rbtree_t *tree)
{
    if (tree->root->color != RB_BLACK)
        return -1;

    int bh;
    return rb_validate_node(tree, tree->root, NULL, NULL, &bh) ? 0 : -1;
}

/* Post-order: free a node's children before the node itself, so no pointer
 * into freed memory is ever dereferenced. Stops at t->nil, the shared
 * sentinel, which rb_destroy frees separately, once, after this returns. */
static void rb_destroy_node(rbtree_t *tree, rbnode_t *target)
{
    if (target == tree->nil)
        return;
    rb_destroy_node(tree, target->left);
    rb_destroy_node(tree, target->right);
    if (tree->value_free)
        tree->value_free(target->value);
    rb_free(target->key);
    rb_free(target);
}

void rb_destroy(rbtree_t *tree)
{
    if (!tree)
        return;
    rb_destroy_node(tree, tree->root);
    rb_free(tree->nil);
    rb_free(tree);
}