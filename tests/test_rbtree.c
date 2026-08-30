#include "rbtree.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    int n;
} collect_ctx_t;

static void collect_cb(const char *key, void *value, void *ctx)
{
    (void)value;
    collect_ctx_t *c = ctx;
    assert(c->n < MAX_COLLECT);
    c->keys[c->n++] = key;
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

        rb_destroy(t);
        printf("ok - insert_overwrite_frees_old_value\n");
    }

    /* Case 6: distinct keys each get their own slot; size accumulates
     * and lookups don't cross-contaminate. */
    {
        rbtree_t *t = rb_create(NULL);
        assert(t);
        assert(rb_insert(t, "a-key", "a-value") == 0);
        assert(rb_insert(t, "m-key", "m-value") == 0);
        assert(rb_insert(t, "z-key", "z-value") == 0);

        assert(rb_size(t) == 3);
        assert(strcmp(rb_find(t, "a-key"), "a-value") == 0);
        assert(strcmp(rb_find(t, "m-key"), "m-value") == 0);
        assert(strcmp(rb_find(t, "z-key"), "z-value") == 0);
        assert(rb_find(t, "q-key") == NULL);

        rb_destroy(t);
        printf("ok - insert_distinct_keys_accumulate_size\n");
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

        rb_destroy(t);
        printf("ok - insert_rebalances_descending_run\n");
    }
}

int main(void)
{
    test_rb_create();
    test_rb_insert();
    return 0;
}