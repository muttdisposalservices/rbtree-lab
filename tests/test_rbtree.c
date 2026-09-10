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

/* Distinguishes "freed once" from "freed twice" (a real double free), which
 * was_freed's plain membership check can't tell apart. */
static int free_occurrences(void *value)
{
    int n = 0;
    for (int i = 0; i < freed_count; i++)
        if (freed_values[i] == value)
            n++;
    return n;
}

/* Scoped variants of was_freed/free_occurrences: only consider frees
 * recorded at or after `since` (a freed_count high-water mark taken right
 * before the value under test was allocated). Needed because glibc's
 * tcache commonly hands a just-freed same-size chunk back to the very next
 * malloc -- an unscoped scan would false-positive on a *stale* entry for a
 * previous, unrelated allocation that happened to reuse the same address. */
static int was_freed_since(void *value, int since)
{
    for (int i = since; i < freed_count; i++)
        if (freed_values[i] == value)
            return 1;
    return 0;
}

static int free_occurrences_since(void *value, int since)
{
    int n = 0;
    for (int i = since; i < freed_count; i++)
        if (freed_values[i] == value)
            n++;
    return n;
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

/* Every mk_node-built fixture allocates exactly node_count node structs and
 * node_count key buffers; rb_destroy must free all of those plus nil and the
 * tree struct itself -- no more (a leak) and no less (an over-free, or nil
 * getting freed more than once if traversal doesn't stop at the sentinel). */
static void destroy_and_check_frees(rbtree_t *t, int node_count)
{
    fault_malloc_arm(0);          /* never fails; just resets the free counter */
    rb_destroy(t);
    assert(fault_malloc_free_count() == node_count * 2 + 2);
    fault_malloc_disarm();
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
            destroy_and_check_frees(t, 3);
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
        destroy_and_check_frees(t, 2);
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
        destroy_and_check_frees(t, 2);
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
        destroy_and_check_frees(t, 3);
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
        destroy_and_check_frees(t, 3);
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
        destroy_and_check_frees(t, 2);
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
        destroy_and_check_frees(t, 2);
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
        destroy_and_check_frees(t, 1);
        printf("ok - validate_root_must_be_black\n");
    }
}

typedef struct {
    const char *name;
    const char *key;
    void *value;
    int fail_at;           /* 0 = no injected malloc failure */
    int expect_ok;         /* expect rb_insert to return 0 */
    int expect_free_count; /* rb_free calls during the faulted insert; -1 = n/a */
} insert_case_t;

/* Cases 1-3: baseline success, and injected failure on each of
 * rb_insert's internal allocations. Order-agnostic about which
 * allocation (node struct vs. key copy) fail_at hits -- the header's
 * -1 contract must hold identically either way.
 *
 * expect_free_count mirrors create_cases' reasoning: fail_at=1 fails on the
 * very first rb_malloc call (the node struct), before anything else is
 * allocated, so cleanup frees nothing (0); fail_at=2 fails on the second call
 * (the key copy) after the node struct already succeeded, so cleanup frees
 * exactly that one allocation (1). */
static const insert_case_t insert_cases[] = {
    { "insert_into_empty_success", "alpha", "alpha-value", 0, 1, -1 },
    { "insert_fails_first_alloc",  "beta",  "beta-value",  1, 0,  0 },
    { "insert_fails_second_alloc", "gamma", "gamma-value", 2, 0,  1 },
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
        if (c->fail_at) {
            assert(fault_malloc_free_count() == c->expect_free_count);
            fault_malloc_disarm();
        }

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
        assert(free_occurrences(old_value) == 1); /* not freed twice */
        assert(!was_freed(new_value));
        assert(rb_find(t, "dup") == new_value);

        /* A second overwrite of the same key, armed so the very first
         * rb_malloc call would fail: overwriting an existing key must not
         * allocate a new node or key copy (the key is already stored), so a
         * correct rb_insert makes zero rb_malloc/rb_free calls here and
         * still returns 0 despite the arming. An implementation that always
         * allocates a fresh node on every insert (even for a duplicate key)
         * would either fail here or leak the discarded new node/key. */
        int since = freed_count;   /* old_value's slot may get reused below */
        char *third_value = malloc(8);
        assert(third_value);
        strcpy(third_value, "third");
        fault_malloc_arm(1);
        int rc = rb_insert(t, "dup", third_value);
        assert(fault_malloc_free_count() == 0);
        fault_malloc_disarm();
        assert(rc == 0);

        assert(rb_size(t) == 1);
        assert(free_occurrences_since(new_value, since) == 1); /* freed once, not twice */
        assert(!was_freed_since(third_value, since));
        assert(rb_find(t, "dup") == third_value);

        /* A couple more surviving entries, so destroy has to walk multiple
         * nodes, not just the one left over from the overwrites. */
        char *extra1 = malloc(8);
        char *extra2 = malloc(8);
        assert(extra1 && extra2);
        strcpy(extra1, "extra1");
        strcpy(extra2, "extra2");
        assert(rb_insert(t, "extra-key-1", extra1) == 0);
        assert(rb_insert(t, "extra-key-2", extra2) == 0);

        /* The repeated overwrite must have updated the existing node in
         * place, not left a stale second "dup" node behind: exactly 3
         * distinct keys should be visible to both validate and foreach. */
        assert(rb_validate(t) == 0);
        collect_ctx_t after_overwrite = {0};
        rb_foreach(t, collect_cb, &after_overwrite);
        assert(after_overwrite.n == 3);

        rb_destroy(t);
        /* rb_destroy must run value_free on every value still owned by the
         * tree, not just the ones already freed by the earlier overwrites. */
        assert(was_freed(third_value));
        assert(was_freed(extra1));
        assert(was_freed(extra2));
        assert(freed_count == 5); /* old_value + new_value (overwrites) + 3 by destroy */
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

    /* Case 9: a minimal LL line -- "c" becomes the black root; "b" inserts
     * red as its left child with the fixup loop not running (parent is the
     * root); "a" then inserts red as "b"'s left child, an outer grandchild
     * (a "line", not a triangle), forcing a single right-rotation at "c"
     * with no preceding rotation at "b". Mirrored by Case 10 below; together
     * they isolate the straight-line half of the fixup logic to three nodes
     * apiece, the same way cases 11-12 isolate the zig-zag half. */
    {
        static const char *keys[] = { "c", "b", "a" };
        /* sorted[] references keys[]'s own elements (not fresh literals) so
         * the value-pointer check below compares against the exact pointer
         * that was inserted, not a look-alike duplicate. */
        const char *sorted[] = { keys[2], keys[1], keys[0] }; /* a, b, c */
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

        /* White-box: pin the exact post-rotation shape/colors, not just
         * validate()'s aggregate invariants. */
        rbnode_t *b = t->root;
        assert(strcmp(b->key, "b") == 0 && b->color == RB_BLACK);
        assert(strcmp(b->left->key, "a") == 0 && b->left->color == RB_RED);
        assert(strcmp(b->right->key, "c") == 0 && b->right->color == RB_RED);

        rb_destroy(t);
        printf("ok - insert_rebalances_ll_line\n");
    }

    /* Case 10: a minimal RR line, the mirror of case 9 -- "a" becomes the
     * black root; "b" inserts red as its right child; "c" then inserts red
     * as "b"'s right child, an outer grandchild, forcing a single
     * left-rotation at "a". This is the exact shape that exposed the
     * mirror-side Case 3 bug during review (wrong node recolored black,
     * grandparent recolored black instead of red, and rotate_right fired
     * instead of rotate_left) -- keep this case even if case 7's larger
     * ascending run is ever trimmed, since it isolates that failure to
     * three nodes and a direct structural check below. */
    {
        static const char *keys[] = { "a", "b", "c" };
        const char *sorted[] = { keys[0], keys[1], keys[2] }; /* a, b, c */
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

        rbnode_t *b = t->root;
        assert(strcmp(b->key, "b") == 0 && b->color == RB_BLACK);
        assert(strcmp(b->left->key, "a") == 0 && b->left->color == RB_RED);
        assert(strcmp(b->right->key, "c") == 0 && b->right->color == RB_RED);

        rb_destroy(t);
        printf("ok - insert_rebalances_rr_line\n");
    }

    /* Case 11: a minimal LR zig-zag. "c" becomes the black root; "a" inserts
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

    /* Case 12: a minimal RL zig-zag, the mirror of case 11. "a" becomes the
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

    /* Case 13: a non-monotonic, mixed-order run forcing a combination of
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

    /* Case 14: overwriting an existing key when value_free is NULL must not
     * crash by calling through a NULL function pointer, and must still
     * replace the stored value. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        char value_a[] = "A", value_b[] = "B";

        assert(rb_insert(t, "x", value_a) == 0);
        assert(rb_insert(t, "x", value_b) == 0);

        assert(rb_size(t) == 1);
        assert(rb_find(t, "x") == value_b);

        rb_destroy(t);
        printf("ok - insert_duplicate_key_null_value_free\n");
    }

    /* Case 15: the caller's key buffer is freed immediately after insert
     * returns. Unlike the memset-based key-copy check above (which only
     * catches the tree storing a pointer that later gets mutated), freeing
     * the buffer outright means any lingering pointer into it is a genuine
     * use-after-free that ASan/valgrind will catch, not just a stale value. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);

        char *heap_key = malloc(6);
        assert(heap_key);
        strcpy(heap_key, "epsln");
        assert(rb_insert(t, heap_key, "epsilon-value") == 0);
        free(heap_key);

        assert(rb_size(t) == 1);
        assert(strcmp(rb_find(t, "epsln"), "epsilon-value") == 0);

        collect_ctx_t collected = {0};
        rb_foreach(t, collect_cb, &collected);
        assert(collected.n == 1);
        assert(strcmp(collected.keys[0], "epsln") == 0);

        rb_destroy(t);
        printf("ok - insert_key_survives_caller_free\n");
    }

    /* Case 16: a multi-level recolor. Inserting "m","f","t" leaves m black
     * (root-forced) with f and t red (no fixup loop runs, since m is
     * black). Inserting "d" as f's left child creates a red-red violation
     * (d red, parent f red) with a red uncle (t) -- Case 1 recolors f and t
     * black and m red, then the loop's next iteration sees m's parent is
     * nil (black) and stops, leaving m red until the trailing
     * `t->root->color = RB_BLACK` forces it back to black. Inserting "h" as
     * f's right child then hits no violation at all (f is black by then).
     * This pins down that the final forced-black-root line is load-bearing,
     * not a no-op -- deleting it would leave m red here, and rb_validate
     * would catch it, but this also checks the exact resulting shape/colors
     * a validate()-only check could miss if two bugs happened to cancel out. */
    {
        static const char *keys[] = { "m", "f", "t", "d", "h" };
        rbtree_t *t = rb_create(NULL);
        assert(t);
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            assert(rb_insert(t, keys[i], (void *)keys[i]) == 0);
        assert(rb_size(t) == 5);
        assert(rb_validate(t) == 0);

        rbnode_t *m = t->root;
        assert(strcmp(m->key, "m") == 0 && m->color == RB_BLACK);
        assert(m->parent == t->nil);

        rbnode_t *f = m->left, *tt = m->right;
        assert(strcmp(f->key, "f") == 0 && f->color == RB_BLACK && f->parent == m);
        assert(strcmp(tt->key, "t") == 0 && tt->color == RB_BLACK && tt->parent == m);
        assert(tt->left == t->nil && tt->right == t->nil);

        rbnode_t *d = f->left, *h = f->right;
        assert(strcmp(d->key, "d") == 0 && d->color == RB_RED && d->parent == f);
        assert(strcmp(h->key, "h") == 0 && h->color == RB_RED && h->parent == f);
        assert(d->left == t->nil && d->right == t->nil);
        assert(h->left == t->nil && h->right == t->nil);

        collect_ctx_t collected = {0};
        rb_foreach(t, collect_cb, &collected);
        static const char *sorted[] = { "d", "f", "h", "m", "t" };
        assert(collected.n == 5);
        for (int i = 0; i < collected.n; i++)
            assert(strcmp(collected.keys[i], sorted[i]) == 0);

        rb_destroy(t);
        printf("ok - insert_fixup_recolor_reaches_root\n");
    }

    /* Case 17: rb_destroy must free every real rb_insert-built node (struct
     * + key copy) plus nil and the tree struct, and must run value_free on
     * every still-owned value, across a tree actually shaped by fixup
     * rotations -- not just the mk_node fixtures destroy_and_check_frees's
     * other callers use. */
    {
        rbtree_t *t = rb_create(track_free);
        assert(t);
        reset_tracking();

        static const char *keys[] = { "delta", "alpha", "gamma", "beta" };
        size_t n = sizeof keys / sizeof keys[0];
        void *values[4];
        for (size_t i = 0; i < n; i++) {
            values[i] = malloc(8);
            assert(values[i]);
            strcpy(values[i], keys[i]);
            assert(rb_insert(t, keys[i], values[i]) == 0);
        }
        assert(rb_size(t) == n);
        assert(rb_validate(t) == 0);

        fault_malloc_arm(0);          /* never fails; just resets the free counter */
        rb_destroy(t);
        assert(fault_malloc_free_count() == (int)n * 2 + 2);
        fault_malloc_disarm();

        for (size_t i = 0; i < n; i++)
            assert(was_freed(values[i]));
        assert(freed_count == (int)n);
        printf("ok - insert_destroy_frees_real_nodes\n");
    }
}

/* rb_delete must locate the node by key (returning -1 untouched if
 * absent), then splice it out via the three classic BST cases, freeing
 * exactly the removed node's key copy and (if owned) its value. No RB
 * fixup exists yet, so these cases only exercise structural correctness,
 * not rb_validate. */
static void test_bst_delete(void)
{
    /* Case 1: absent key, empty tree. Tree is untouched. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        assert(rb_delete(t, "ghost") == -1);
        assert(rb_size(t) == 0);
        assert(rb_validate(t) == 0);
        rb_destroy(t);
        printf("ok - delete_absent_key_empty_tree\n");
    }

    /* Case 2: absent key, nonempty tree. Every existing key must still be
     * reachable afterward. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        static const char *keys[] = { "m", "f", "t", "d", "h" };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            assert(rb_insert(t, keys[i], (void *)keys[i]) == 0);

        assert(rb_delete(t, "zzz") == -1);
        assert(rb_size(t) == 5);
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            assert(rb_find(t, keys[i]) == (void *)keys[i]);

        rb_destroy(t);
        printf("ok - delete_absent_key_nonempty_tree\n");
    }

    /* Case 3: deleting the only node empties the tree, decrements size to
     * 0, frees the key and (via value_free) the value, and leaves
     * t->root pointing at t->nil. */
    {
        rbtree_t *t = rb_create(track_free);
        assert(t);
        reset_tracking();
        char *value = malloc(8);
        assert(value);
        strcpy(value, "only");
        assert(rb_insert(t, "solo", value) == 0);

        fault_malloc_arm(0);
        assert(rb_delete(t, "solo") == 0);
        assert(fault_malloc_free_count() == 2); /* key copy + node struct */
        fault_malloc_disarm();

        assert(rb_size(t) == 0);
        assert(rb_find(t, "solo") == NULL);
        assert(t->root == t->nil);
        assert(was_freed(value));
        assert(free_occurrences(value) == 1);

        rb_destroy(t);
        printf("ok - delete_only_node_empties_tree\n");
    }

    /* Case 4: deleting a leaf (no children). Hits the target->left ==
     * t->nil branch with a t->nil replacement. Built directly with
     * mk_node (bypassing rb_insert/fixup) so the shape is pinned exactly,
     * same technique test_rb_validate's fixtures use. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_BLACK);
        rbnode_t *left = mk_node(t, "f", RB_BLACK);
        rbnode_t *right = mk_node(t, "t", RB_BLACK);
        root->left = left; root->right = right;
        root->parent = t->nil;
        left->parent = root; right->parent = root;
        t->root = root;
        t->size = 3;

        fault_malloc_arm(0);
        assert(rb_delete(t, "f") == 0);
        assert(fault_malloc_free_count() == 2);
        fault_malloc_disarm();

        assert(rb_size(t) == 2);
        assert(root->left == t->nil);
        assert(root->right == right);

        destroy_and_check_frees(t, 2);
        printf("ok - delete_leaf_no_children\n");
    }

    /* Case 5: only a left child. Hits the target->right == t->nil branch,
     * promoting target->left into target's slot. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_BLACK);
        rbnode_t *left = mk_node(t, "f", RB_BLACK);
        rbnode_t *grandchild = mk_node(t, "b", RB_BLACK);
        root->left = left; root->right = t->nil;
        root->parent = t->nil;
        left->parent = root;
        left->left = grandchild; left->right = t->nil;
        grandchild->parent = left;
        t->root = root;
        t->size = 3;

        fault_malloc_arm(0);
        assert(rb_delete(t, "f") == 0);
        assert(fault_malloc_free_count() == 2);
        fault_malloc_disarm();

        assert(rb_size(t) == 2);
        assert(root->left == grandchild);
        assert(grandchild->parent == root);
        assert(grandchild->left == t->nil && grandchild->right == t->nil);

        destroy_and_check_frees(t, 2);
        printf("ok - delete_node_with_only_left_child\n");
    }

    /* Case 6: only a right child. Still the target->left == t->nil branch
     * (same as case 4), but this time it promotes a real node instead of
     * t->nil -- the half of that branch case 4 doesn't cover. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_BLACK);
        rbnode_t *target = mk_node(t, "t", RB_BLACK);
        rbnode_t *grandchild = mk_node(t, "v", RB_BLACK);
        root->right = target; root->left = t->nil;
        root->parent = t->nil;
        target->parent = root;
        target->right = grandchild; target->left = t->nil;
        grandchild->parent = target;
        t->root = root;
        t->size = 3;

        fault_malloc_arm(0);
        assert(rb_delete(t, "t") == 0);
        assert(fault_malloc_free_count() == 2);
        fault_malloc_disarm();

        assert(rb_size(t) == 2);
        assert(root->right == grandchild);
        assert(grandchild->parent == root);
        assert(grandchild->left == t->nil && grandchild->right == t->nil);

        destroy_and_check_frees(t, 2);
        printf("ok - delete_node_with_only_right_child\n");
    }

    /* Case 7: two children, and the in-order successor is target's
     * immediate right child (it has no left child of its own), so
     * rb_inorder_successor returns it directly and successor->parent ==
     * target -- rb_delete must skip the detach-from-its-own-spot step and
     * just move the successor into target's slot along with target's left
     * subtree. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_BLACK);
        rbnode_t *sibling = mk_node(t, "d", RB_BLACK);
        rbnode_t *target = mk_node(t, "t", RB_BLACK);
        rbnode_t *targetLeft = mk_node(t, "p", RB_BLACK);
        rbnode_t *successor = mk_node(t, "v", RB_BLACK);
        root->left = sibling; root->right = target;
        root->parent = t->nil;
        sibling->parent = root;
        target->parent = root;
        target->left = targetLeft; target->right = successor;
        targetLeft->parent = target;
        successor->parent = target;
        t->root = root;
        t->size = 5;

        fault_malloc_arm(0);
        assert(rb_delete(t, "t") == 0);
        assert(fault_malloc_free_count() == 2);
        fault_malloc_disarm();

        assert(rb_size(t) == 4);
        assert(root->right == successor);
        assert(successor->parent == root);
        assert(successor->left == targetLeft);
        assert(targetLeft->parent == successor);
        assert(successor->right == t->nil); /* successor's own right, untouched */

        destroy_and_check_frees(t, 4);
        printf("ok - delete_two_children_successor_is_right_child\n");
    }

    /* Case 8: two children, and the in-order successor is deeper inside
     * target's right subtree (successor->parent != target). rb_delete
     * must first splice the successor out of its own spot (promoting the
     * successor's right child -- a minimum node never has a left child),
     * then move the successor into target's slot with BOTH of target's
     * original children reattached. The successor is given its own right
     * child here so that promotion is actually exercised, not just left
     * nil by coincidence. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        rbnode_t *root = mk_node(t, "m", RB_BLACK);
        rbnode_t *sibling = mk_node(t, "d", RB_BLACK);
        rbnode_t *target = mk_node(t, "t", RB_BLACK);
        rbnode_t *targetLeft = mk_node(t, "p", RB_BLACK);
        rbnode_t *targetRight = mk_node(t, "x", RB_BLACK);
        rbnode_t *successor = mk_node(t, "w", RB_BLACK);
        rbnode_t *successorRightChild = mk_node(t, "u", RB_BLACK);
        rbnode_t *targetRightRight = mk_node(t, "y", RB_BLACK);

        root->left = sibling; root->right = target;
        root->parent = t->nil;
        sibling->parent = root;
        target->parent = root;
        target->left = targetLeft; target->right = targetRight;
        targetLeft->parent = target;
        targetRight->parent = target;
        targetRight->left = successor; targetRight->right = targetRightRight;
        successor->parent = targetRight;
        targetRightRight->parent = targetRight;
        successor->left = t->nil; successor->right = successorRightChild;
        successorRightChild->parent = successor;
        t->root = root;
        t->size = 8;

        fault_malloc_arm(0);
        assert(rb_delete(t, "t") == 0);
        assert(fault_malloc_free_count() == 2);
        fault_malloc_disarm();

        assert(rb_size(t) == 7);
        assert(root->right == successor);
        assert(successor->parent == root);
        assert(successor->left == targetLeft && targetLeft->parent == successor);
        assert(successor->right == targetRight && targetRight->parent == successor);
        assert(targetRight->left == successorRightChild);
        assert(successorRightChild->parent == targetRight);
        assert(targetRight->right == targetRightRight);
        assert(targetRightRight->parent == targetRight);

        destroy_and_check_frees(t, 7);
        printf("ok - delete_two_children_successor_is_deep\n");
    }

    /* Case 9: end-to-end sanity check against a tree assembled through the
     * public API (real fixup shapes it, not a hand-built fixture),
     * deleting several keys of differing shapes in sequence and confirming
     * size, findability, in-order structure, and value ownership all stay
     * consistent -- not just the pointer-level mechanics cases 4-8 pin
     * down. */
    {
        rbtree_t *t = rb_create(track_free);
        assert(t);
        reset_tracking();

        static const char *keys[] = { "m", "f", "t", "d", "h", "p", "s", "v", "b" };
        size_t n = sizeof keys / sizeof keys[0];
        void *values[9];
        for (size_t i = 0; i < n; i++) {
            values[i] = malloc(8);
            assert(values[i]);
            strcpy(values[i], keys[i]);
            assert(rb_insert(t, keys[i], values[i]) == 0);
        }
        assert(rb_size(t) == n);

        static const char *to_delete[] = { "b", "h", "t" };
        for (size_t i = 0; i < sizeof to_delete / sizeof to_delete[0]; i++) {
            void *deleted_value = rb_find(t, to_delete[i]);
            assert(deleted_value != NULL);
            assert(rb_delete(t, to_delete[i]) == 0);
            assert(rb_find(t, to_delete[i]) == NULL);
            assert(was_freed(deleted_value));
            assert(free_occurrences(deleted_value) == 1);
        }

        assert(rb_size(t) == n - 3);

        collect_ctx_t collected = {0};
        rb_foreach(t, collect_cb, &collected);
        assert(collected.n == (int)(n - 3));
        /* invariant: in-order traversal is still strictly ascending;
         * deletion must never leave BST ordering broken. */
        for (int i = 0; i + 1 < collected.n; i++)
            assert(strcmp(collected.keys[i], collected.keys[i + 1]) < 0);

        for (size_t i = 0; i < n; i++) {
            int deleted = 0;
            for (size_t j = 0; j < sizeof to_delete / sizeof to_delete[0]; j++)
                if (strcmp(keys[i], to_delete[j]) == 0) deleted = 1;
            if (!deleted)
                assert(rb_find(t, keys[i]) == values[i]);
        }

        rb_destroy(t);
        /* rb_destroy must free value_free on every value still owned -- the
         * 3 already deleted plus the 6 remaining. */
        assert(freed_count == (int)n);
        printf("ok - delete_end_to_end_mixed_shapes\n");
    }
}

int main(void)
{
    test_rb_validate();
    test_rb_create();
    test_rb_insert();
    test_bst_delete();
    return 0;
}