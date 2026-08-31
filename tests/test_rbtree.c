#include "rbtree.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* White-box access to rbnode_t / struct rbtree for the invalid-tree fixtures
 * in test_rb_validate below -- rbtree_t stays opaque in rbtree.h by design,
 * but rb_validate's negative cases can't be built through the public API
 * (rb_insert should never itself produce an invalid tree). See Makefile:
 * $(SRC) is deliberately not compiled into $(BIN) separately, to avoid
 * double-defining the symbols this pulls in. */
#include "../src/rbtree.c"

/* Provided by tests/fault_malloc.c: forces a specific rb_malloc call inside
 * rb_create to return NULL, without touching libc malloc. */
extern void fault_malloc_arm(int nth_call_to_fail);
extern void fault_malloc_disarm(void);
extern int fault_malloc_free_count(void);

static int value_free_calls = 0;

static void counting_free(void *value)
{
    (void)value;
    value_free_calls++;
}

static void count_cb(const char *key, void *value, void *ctx)
{
    (void)key;
    (void)value;
    (*(int *)ctx)++;
}

/* Tracks which value pointers the tree has actually freed, so insert tests
 * can tell "old value freed on overwrite" apart from "value untouched on
 * allocation failure" instead of just trusting the return code. */
#define MAX_FREED 16
static void *freed_values[MAX_FREED];
static int freed_count;

static void reset_tracking(void)
{
    freed_count = 0;
}

static void track_free(void *value)
{
    assert(freed_count < MAX_FREED);
    freed_values[freed_count++] = value;
    free(value);
}

static int was_freed(void *value)
{
    for (int i = 0; i < freed_count; i++)
        if (freed_values[i] == value)
            return 1;
    return 0;
}

#define MAX_COLLECT 16
typedef struct {
    const char *keys[MAX_COLLECT];
    void *values[MAX_COLLECT];
    int n;
} collect_ctx_t;

static void collect_cb(const char *key, void *value, void *ctx)
{
    collect_ctx_t *c = ctx;
    assert(c->n < MAX_COLLECT);
    c->keys[c->n] = key;
    c->values[c->n] = value;
    c->n++;
}

typedef struct {
    const char *name;
    rb_value_free_fn value_free;
    int fail_at;          /* 0 = no injected malloc failure */
    int expect_nonnull;
    int expect_free_count; /* rb_free calls during the faulted create; -1 = n/a */
} create_case_t;

/* Baseline success (with/without value_free), and injected failure on
 * each of rb_create's two allocations (the struct, then the nil
 * sentinel). Each row that returns non-NULL also exercises
 * destroy-after-success being crash-free and memcheck-clean, and now
 * checks rb_validate and that value_free is never spuriously invoked
 * on an empty tree.
 *
 * fail_at assumes rb_create makes exactly two rb_malloc calls, in this
 * order: the tree struct, then the nil sentinel. If that allocation
 * strategy changes, these cases will fail loudly via expect_nonnull
 * rather than silently no-op. expect_free_count values: fail_at=1
 * fails before anything is allocated, so cleanup frees nothing (0);
 * fail_at=2 fails after the tree struct succeeds, so cleanup frees
 * exactly that one allocation (1). */
static const create_case_t create_cases[] = {
    { "create_null_value_free",    NULL,          0, 1, -1 },
    { "create_nonnull_value_free", counting_free, 0, 1, -1 },
    { "create_fail_first_alloc",   NULL,          1, 0,  0 },
    { "create_fail_second_alloc",  NULL,          2, 0,  1 },
};

static void test_rb_create(void)
{
    size_t n = sizeof create_cases / sizeof create_cases[0];
    /* invariant: each iteration leaves no allocation armed and no tree
     * leaked, regardless of whether rb_create succeeded. */
    for (size_t i = 0; i < n; i++) {
        const create_case_t *c = &create_cases[i];

        if (c->fail_at)
            fault_malloc_arm(c->fail_at);
        rbtree_t *t = rb_create(c->value_free);
        if (c->fail_at) {
            assert(fault_malloc_free_count() == c->expect_free_count);
            fault_malloc_disarm();
        }

        assert((t != NULL) == c->expect_nonnull);

        if (t) {
            assert(rb_size(t) == 0);
            assert(rb_validate(t) == 0);
            assert(rb_find(t, "probe") == NULL);
            int calls = 0;
            rb_foreach(t, count_cb, &calls);
            assert(calls == 0);
            value_free_calls = 0;
            rb_destroy(t);
            assert(value_free_calls == 0);
        }
        printf("ok - %s\n", c->name);
    }

    /* Two live trees don't share hidden state. */
    {
        rbtree_t *a = rb_create(NULL);
        rbtree_t *b = rb_create(NULL);
        assert(a && b && a != b);
        assert(rb_size(a) == 0 && rb_size(b) == 0);
        assert(rb_validate(a) == 0 && rb_validate(b) == 0);
        rb_destroy(a);
        assert(rb_size(b) == 0);
        rb_destroy(b);
        printf("ok - create_independent_instances\n");
    }

    /* rb_destroy is documented NULL-safe. */
    {
        rb_destroy(NULL);
        printf("ok - create_destroy_null_safe\n");
    }
}

/* White-box fixture helper: builds one node directly (bypassing rb_insert)
 * so test_rb_validate can wire up structurally-valid-but-invariant-violating
 * trees that rb_insert itself should never be able to produce. */
static rbnode_t *mk_node(rbtree_t *t, const char *key, int color)
{
    rbnode_t *n = rb_malloc(sizeof *n);
    assert(n);
    n->key = rb_malloc(strlen(key) + 1);
    assert(n->key);
    memcpy(n->key, key, strlen(key) + 1);
    n->value = NULL;
    n->left = n->right = n->parent = t->nil;
    n->color = color;
    return n;
}

/* rb_validate must enforce four invariants: BST order, red implies black
 * children, equal black-height on every root-to-nil path, and the root is
 * black. Each case below isolates exactly one of those, and every violation
 * gets its left/right (or shallow/deep) mirror so an implementation that
 * only checks one side can't pass by accident. */
static void test_rb_validate(void)
{
    /* Cases 1-4: red-red violation (a red node with a red child), at each
     * combination of which side of the root the red parent is on and which
     * side of that parent the red child is on. Ordering is correct and both
     * root-to-nil paths carry the same black count in every case, so only
     * the red-red rule is broken. */
    {
        static const struct {
            const char *name;
            const char *parent_key;
            int parent_is_left;      /* parent is root's left (1) or right (0) child */
            const char *child_key;
            int child_is_left;       /* red child is parent's left (1) or right (0) child */
        } cases[] = {
            { "validate_red_red_LL", "d", 1, "b", 1 },
            { "validate_red_red_LR", "d", 1, "f", 0 },
            { "validate_red_red_RL", "t", 0, "r", 1 },
            { "validate_red_red_RR", "t", 0, "v", 0 },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            rbtree_t *t = rb_create(NULL);
            assert(t);

            rbnode_t *parent = mk_node(t, cases[i].parent_key, RB_RED);
            rbnode_t *child = mk_node(t, cases[i].child_key, RB_RED);
            child->parent = parent;
            if (cases[i].child_is_left)
                parent->left = child;
            else
                parent->right = child;

            rbnode_t *root = mk_node(t, "m", RB_BLACK);
            root->parent = t->nil;
            if (cases[i].parent_is_left) {
                root->left = parent;
                root->right = t->nil;
            } else {
                root->right = parent;
                root->left = t->nil;
            }
            parent->parent = root;
            t->root = root;
            t->size = 3;

            assert(rb_validate(t) != 0);
            rb_destroy(t);
            printf("ok - %s\n", cases[i].name);
        }
    }

    /* Cases 5-8: BST-order violation. The first two are immediate
     * parent/child inversions; the last two are deeper ancestor-bound
     * violations -- a node that's fine relative to its immediate parent but
     * violates a grandparent's bound, which a validator that only compares
     * a node to its direct parent (instead of threading a min/max bound)
     * would miss. All nodes BLACK so there's no red-red/black-height noise. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_BLACK);
        rbnode_t *left = mk_node(t, "z", RB_BLACK); /* should be < "m" */
        root->left = left;
        root->right = t->nil;
        root->parent = t->nil;
        left->parent = root;
        t->root = root;
        t->size = 2;
        assert(rb_validate(t) != 0);
        rb_destroy(t);
        printf("ok - validate_bst_order_immediate_left_too_big\n");
    }
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_BLACK);
        rbnode_t *right = mk_node(t, "b", RB_BLACK); /* should be > "m" */
        root->right = right;
        root->left = t->nil;
        root->parent = t->nil;
        right->parent = root;
        t->root = root;
        t->size = 2;
        assert(rb_validate(t) != 0);
        rb_destroy(t);
        printf("ok - validate_bst_order_immediate_right_too_small\n");
    }
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_BLACK);
        rbnode_t *left = mk_node(t, "d", RB_BLACK);       /* valid: "d" < "m" */
        rbnode_t *deep = mk_node(t, "x", RB_BLACK);       /* valid vs "d": "x" > "d" ... */
        left->right = deep;                               /* ...but "x" > "m" violates root's bound */
        left->left = t->nil;
        deep->left = deep->right = t->nil;
        root->left = left;
        root->right = t->nil;
        root->parent = t->nil;
        left->parent = root;
        deep->parent = left;
        t->root = root;
        t->size = 3;
        assert(rb_validate(t) != 0);
        rb_destroy(t);
        printf("ok - validate_bst_order_deep_left_exceeds_root\n");
    }
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_BLACK);
        rbnode_t *right = mk_node(t, "t", RB_BLACK);      /* valid: "t" > "m" */
        rbnode_t *deep = mk_node(t, "c", RB_BLACK);       /* valid vs "t": "c" < "t" ... */
        right->left = deep;                                /* ...but "c" < "m" violates root's bound */
        right->right = t->nil;
        deep->left = deep->right = t->nil;
        root->right = right;
        root->left = t->nil;
        root->parent = t->nil;
        right->parent = root;
        deep->parent = right;
        t->root = root;
        t->size = 3;
        assert(rb_validate(t) != 0);
        rb_destroy(t);
        printf("ok - validate_bst_order_deep_right_below_root\n");
    }

    /* Cases 9-10: black-height mismatch. All nodes BLACK, correct BST
     * order, differing only in which side carries an extra black node. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_BLACK);
        rbnode_t *left = mk_node(t, "b", RB_BLACK);
        left->left = left->right = t->nil;
        root->left = left;
        root->right = t->nil; /* one black shorter than the left path */
        root->parent = t->nil;
        left->parent = root;
        t->root = root;
        t->size = 2;
        assert(rb_validate(t) != 0);
        rb_destroy(t);
        printf("ok - validate_black_height_left_heavy\n");
    }
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_BLACK);
        rbnode_t *right = mk_node(t, "v", RB_BLACK);
        right->left = right->right = t->nil;
        root->right = right;
        root->left = t->nil; /* one black shorter than the right path */
        root->parent = t->nil;
        right->parent = root;
        t->root = root;
        t->size = 2;
        assert(rb_validate(t) != 0);
        rb_destroy(t);
        printf("ok - validate_black_height_right_heavy\n");
    }

    /* Case 11: root color. A single red root is valid BST order (only one
     * node), has no red-red violation (no children to conflict with), and
     * has equal (zero-length) black-height on both sides -- the only broken
     * rule is that the root itself must be black. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_RED);
        root->left = root->right = t->nil;
        root->parent = t->nil;
        t->root = root;
        t->size = 1;
        assert(rb_validate(t) != 0);
        rb_destroy(t);
        printf("ok - validate_root_must_be_black\n");
    }
}

typedef struct {
    const char *name;
    const char *key;
    void *value;
    int fail_at;           /* 0 = no injected malloc failure */
    int expect_ok;         /* expect rb_insert to return 0 */
} insert_case_t;

/* Cases 1-3: baseline success, and injected failure on each of
 * rb_insert's internal allocations. Order-agnostic about which
 * allocation (node struct vs. key copy) fail_at hits -- the header's
 * -1 contract must hold identically either way. */
static const insert_case_t insert_cases[] = {
    { "insert_into_empty_success", "alpha", "alpha-value", 0, 1 },
    { "insert_fails_first_alloc",  "beta",  "beta-value",  1, 0 },
    { "insert_fails_second_alloc", "gamma", "gamma-value", 2, 0 },
};

static void test_rb_insert(void)
{
    size_t n = sizeof insert_cases / sizeof insert_cases[0];
    /* invariant: each iteration gets a fresh tree, so a case's outcome
     * never depends on a prior case's inserts. */
    for (size_t i = 0; i < n; i++) {
        const insert_case_t *c = &insert_cases[i];
        rbtree_t *t = rb_create(NULL);
        assert(t);

        if (c->fail_at)
            fault_malloc_arm(c->fail_at);
        int rc = rb_insert(t, c->key, c->value);
        if (c->fail_at)
            fault_malloc_disarm();

        assert((rc == 0) == c->expect_ok);
        assert(rb_size(t) == (size_t)(c->expect_ok ? 1 : 0));
        assert(rb_find(t, c->key) == (c->expect_ok ? c->value : NULL));

        rb_destroy(t);
        printf("ok - %s\n", c->name);
    }

    /* Case 4: on allocation failure the tree must not call value_free,
     * and the caller must retain sole ownership of value. */
    {
        rbtree_t *t = rb_create(track_free);
        assert(t);
        for (int fail_at = 1; fail_at <= 2; fail_at++) {
            reset_tracking();
            char *v = malloc(8);
            assert(v);
            strcpy(v, "owned");

            fault_malloc_arm(fail_at);
            int rc = rb_insert(t, "delta", v);
            fault_malloc_disarm();

            assert(rc == -1);
            assert(!was_freed(v));
            assert(rb_size(t) == 0);
            free(v);   /* caller still owns it; must not double-free */
        }
        rb_destroy(t);
        printf("ok - insert_failure_value_not_consumed\n");
    }

    /* Case 5: overwriting an existing key frees the old value via
     * value_free, keeps size unchanged, and matches by key content (two
     * distinct buffers with identical content, not the same pointer). */
    {
        rbtree_t *t = rb_create(track_free);
        assert(t);
        reset_tracking();

        char key_first[] = "dup";
        char key_second[] = "dup";
        char *old_value = malloc(8);
        char *new_value = malloc(8);
        assert(old_value && new_value);
        strcpy(old_value, "old");
        strcpy(new_value, "new");

        assert(rb_insert(t, key_first, old_value) == 0);
        assert(rb_insert(t, key_second, new_value) == 0);

        assert(rb_size(t) == 1);
        assert(was_freed(old_value));
        assert(!was_freed(new_value));
        assert(rb_find(t, "dup") == new_value);

        /* A couple more surviving entries, so destroy has to walk multiple
         * nodes, not just the one left over from the overwrite. */
        char *extra1 = malloc(8);
        char *extra2 = malloc(8);
        assert(extra1 && extra2);
        strcpy(extra1, "extra1");
        strcpy(extra2, "extra2");
        assert(rb_insert(t, "extra-key-1", extra1) == 0);
        assert(rb_insert(t, "extra-key-2", extra2) == 0);

        rb_destroy(t);
        /* rb_destroy must run value_free on every value still owned by the
         * tree, not just the one already freed by the earlier overwrite. */
        assert(was_freed(new_value));
        assert(was_freed(extra1));
        assert(was_freed(extra2));
        assert(freed_count == 4); /* old_value (overwrite) + 3 freed by destroy */
        printf("ok - insert_overwrite_frees_old_value\n");
    }

    /* Case 6: distinct keys each get their own slot; size accumulates
     * and lookups don't cross-contaminate. Keys are inserted from mutable
     * buffers that get scribbled over right after each insert returns --
     * if rb_insert stored the caller's pointer instead of rb_malloc+memcpy
     * copying it, the lookups below would see the mutated content instead
     * of the original key. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        char buf_a[] = "a-key", buf_m[] = "m-key", buf_z[] = "z-key";
        assert(rb_insert(t, buf_a, "a-value") == 0);
        assert(rb_insert(t, buf_m, "m-value") == 0);
        assert(rb_insert(t, buf_z, "z-value") == 0);
        memset(buf_a, 'X', strlen(buf_a));
        memset(buf_m, 'X', strlen(buf_m));
        memset(buf_z, 'X', strlen(buf_z));

        assert(rb_size(t) == 3);
        assert(strcmp(rb_find(t, "a-key"), "a-value") == 0);
        assert(strcmp(rb_find(t, "m-key"), "m-value") == 0);
        assert(strcmp(rb_find(t, "z-key"), "z-value") == 0);
        assert(rb_find(t, "q-key") == NULL);
        assert(rb_find(t, "XXXXX") == NULL);

        rb_destroy(t);
        printf("ok - insert_distinct_keys_prove_key_copy\n");
    }

    /* Case 7: an ascending run of inserts forces left-rotations during
     * insert-fixup; rebalancing must preserve red-black invariants and
     * BST (sorted in-order) structure. */
    {
        static const char *keys[] = { "a", "b", "c", "d", "e", "f", "g" };
        size_t run_len = sizeof keys / sizeof keys[0];
        rbtree_t *t = rb_create(NULL);
        assert(t);
        /* invariant: every insert in this run succeeds before the next
         * begins, so size == i+1 after the i-th insert. */
        for (size_t i = 0; i < run_len; i++) {
            assert(rb_insert(t, keys[i], (void *)keys[i]) == 0);
            assert(rb_size(t) == i + 1);
        }

        assert(rb_validate(t) == 0);

        collect_ctx_t collected = {0};
        rb_foreach(t, collect_cb, &collected);
        assert(collected.n == (int)run_len);
        /* invariant: in-order traversal is strictly ascending since all
         * inserted keys are distinct. */
        for (int i = 0; i + 1 < collected.n; i++)
            assert(strcmp(collected.keys[i], collected.keys[i + 1]) < 0);
        /* invariant: foreach's value for each slot is exactly the pointer
         * that was inserted for that key (sorted order == insertion order
         * here, since the run was already ascending). */
        for (int i = 0; i < collected.n; i++)
            assert(collected.values[i] == (void *)keys[i]);

        /* Every key must still be reachable by search after the rotations
         * this run triggered, not just present in the in-order walk. */
        for (size_t i = 0; i < run_len; i++)
            assert(rb_find(t, keys[i]) == (void *)keys[i]);

        rb_destroy(t);
        printf("ok - insert_rebalances_ascending_run\n");
    }

    /* Case 8: a descending run of inserts forces right-rotations during
     * insert-fixup; same invariants must hold. */
    {
        static const char *keys[] = { "g", "f", "e", "d", "c", "b", "a" };
        size_t run_len = sizeof keys / sizeof keys[0];
        rbtree_t *t = rb_create(NULL);
        assert(t);
        for (size_t i = 0; i < run_len; i++) {
            assert(rb_insert(t, keys[i], (void *)keys[i]) == 0);
            assert(rb_size(t) == i + 1);
        }

        assert(rb_validate(t) == 0);

        collect_ctx_t collected = {0};
        rb_foreach(t, collect_cb, &collected);
        assert(collected.n == (int)run_len);
        for (int i = 0; i + 1 < collected.n; i++)
            assert(strcmp(collected.keys[i], collected.keys[i + 1]) < 0);
        /* invariant: foreach's value for each slot is exactly the pointer
         * that was inserted for that key; sorted order is the reverse of
         * this run's (descending) insertion order. */
        for (int i = 0; i < collected.n; i++)
            assert(collected.values[i] == (void *)keys[run_len - 1 - i]);

        /* Every key must still be reachable by search after the rotations
         * this run triggered, not just present in the in-order walk. */
        for (size_t i = 0; i < run_len; i++)
            assert(rb_find(t, keys[i]) == (void *)keys[i]);

        rb_destroy(t);
        printf("ok - insert_rebalances_descending_run\n");
    }

    /* Case 9: a minimal LR zig-zag. "c" becomes the black root; "a" inserts
     * red as its left child with the fixup loop not running (parent is the
     * root); "b" then inserts red as "a"'s right child. "a" is red and is
     * "c"'s left child while "b" is "a"'s right child -- the mismatched
     * shape that forces a left-rotation at "a" followed by a right-rotation
     * at "c" (a genuine double rotation, not two independent single ones). */
    {
        static const char *keys[] = { "c", "a", "b" };
        /* sorted[] references keys[]'s own elements (not fresh literals) so
         * the value-pointer check below compares against the exact pointer
         * that was inserted, not a look-alike duplicate. */
        const char *sorted[] = { keys[1], keys[2], keys[0] }; /* a, b, c */
        size_t run_len = sizeof keys / sizeof keys[0];
        rbtree_t *t = rb_create(NULL);
        assert(t);
        for (size_t i = 0; i < run_len; i++) {
            assert(rb_insert(t, keys[i], (void *)keys[i]) == 0);
            assert(rb_size(t) == i + 1);
        }

        assert(rb_validate(t) == 0);

        collect_ctx_t collected = {0};
        rb_foreach(t, collect_cb, &collected);
        assert(collected.n == (int)run_len);
        for (int i = 0; i < collected.n; i++) {
            assert(strcmp(collected.keys[i], sorted[i]) == 0);
            assert(collected.values[i] == (void *)sorted[i]);
        }

        for (size_t i = 0; i < run_len; i++)
            assert(rb_find(t, keys[i]) == (void *)keys[i]);

        rb_destroy(t);
        printf("ok - insert_rebalances_lr_zigzag\n");
    }

    /* Case 10: a minimal RL zig-zag, the mirror of case 9. "a" becomes the
     * black root; "c" inserts red as its right child; "b" then inserts red
     * as "c"'s left child. "c" is "a"'s right child while "b" is "c"'s left
     * child, forcing a right-rotation at "c" then a left-rotation at "a". */
    {
        static const char *keys[] = { "a", "c", "b" };
        const char *sorted[] = { keys[0], keys[2], keys[1] }; /* a, b, c */
        size_t run_len = sizeof keys / sizeof keys[0];
        rbtree_t *t = rb_create(NULL);
        assert(t);
        for (size_t i = 0; i < run_len; i++) {
            assert(rb_insert(t, keys[i], (void *)keys[i]) == 0);
            assert(rb_size(t) == i + 1);
        }

        assert(rb_validate(t) == 0);

        collect_ctx_t collected = {0};
        rb_foreach(t, collect_cb, &collected);
        assert(collected.n == (int)run_len);
        for (int i = 0; i < collected.n; i++) {
            assert(strcmp(collected.keys[i], sorted[i]) == 0);
            assert(collected.values[i] == (void *)sorted[i]);
        }

        for (size_t i = 0; i < run_len; i++)
            assert(rb_find(t, keys[i]) == (void *)keys[i]);

        rb_destroy(t);
        printf("ok - insert_rebalances_rl_zigzag\n");
    }

    /* Case 11: a non-monotonic, mixed-order run forcing a combination of
     * recolors and rotations on both sides of the tree, unlike the purely
     * ascending/descending runs above. */
    {
        static const char *keys[] = {
            /*        0    1    2    3    4    5    6    7    8    9   10 */
            /*        m    f    t    d    h    p    b    e    g    s    v */
            "m", "f", "t", "d", "h", "p", "b", "e", "g", "s", "v"
        };
        /* sorted[] references keys[]'s own elements (not fresh literals) so
         * the value-pointer check below compares against the exact pointer
         * that was inserted: b d e f g h m p s t v */
        const char *sorted[] = {
            keys[6], keys[3], keys[7], keys[1], keys[8],
            keys[4], keys[0], keys[5], keys[9], keys[2], keys[10]
        };
        size_t run_len = sizeof keys / sizeof keys[0];
        rbtree_t *t = rb_create(NULL);
        assert(t);
        for (size_t i = 0; i < run_len; i++) {
            assert(rb_insert(t, keys[i], (void *)keys[i]) == 0);
            assert(rb_size(t) == i + 1);
        }

        assert(rb_validate(t) == 0);

        collect_ctx_t collected = {0};
        rb_foreach(t, collect_cb, &collected);
        assert(collected.n == (int)run_len);
        for (int i = 0; i < collected.n; i++) {
            assert(strcmp(collected.keys[i], sorted[i]) == 0);
            assert(collected.values[i] == (void *)sorted[i]);
        }

        for (size_t i = 0; i < run_len; i++)
            assert(rb_find(t, keys[i]) == (void *)keys[i]);

        rb_destroy(t);
        printf("ok - insert_rebalances_mixed_order\n");
    }
}

int main(void)
{
    test_rb_validate();
    test_rb_create();
    test_rb_insert();
    return 0;
}