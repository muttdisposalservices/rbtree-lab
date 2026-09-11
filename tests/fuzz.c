#include "rbtree.h"
#include "../src/rb_alloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Distinct keys the fuzzer draws from; kept well below the operation count
 * so inserts/finds/deletes collide often and repeatedly exercise fixup. */
#define KEYSPACE 4096
/* rb_validate and the foreach cross-check both walk the whole tree, so they
 * aren't run every iteration; every 100 ops still keeps a broken invariant
 * within one op's blast radius of an assertion failure. */
#define VALIDATE_INTERVAL 100

#define MIN_SHORT_KEY 1
#define MAX_SHORT_KEY 8
#define MIN_LONG_KEY 9
#define MAX_LONG_KEY 500

static char *key_pool[KEYSPACE];

static int rand_len(void)
{
    return (rand() % 2 == 0)
        ? MIN_SHORT_KEY + rand() % (MAX_SHORT_KEY - MIN_SHORT_KEY + 1)
        : MIN_LONG_KEY + rand() % (MAX_LONG_KEY - MIN_LONG_KEY + 1);
}

enum {
    KEY_NORMAL,
    KEY_SPECIAL_CHARS,
    KEY_REPEATED_CHAR,
    KEY_PREFIX_OF_PREV,
    KEY_RAW_BYTES,
    KEY_CATEGORY_COUNT
};

/* Builds pool entry i. Most categories are adversarial: KEY_SPECIAL_CHARS
 * uses only punctuation/whitespace bytes, KEY_REPEATED_CHAR forces long
 * common-prefix strcmp comparisons, KEY_PREFIX_OF_PREV creates deliberate
 * prefix/extension relationships against the previous entry (exercising the
 * lo/hi key-order boundaries in rb_validate_node), and KEY_RAW_BYTES uses
 * the full non-NUL byte range to catch any signed-char assumption in a
 * comparison, since strcmp must compare as unsigned char. */
static char *make_pool_key(int i)
{
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static const char special_charset[] =
        " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\t";
    static const char mixed_charset[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\t";
    int category = (i == 0) ? KEY_NORMAL : rand() % KEY_CATEGORY_COUNT;

    if (category == KEY_PREFIX_OF_PREV) {
        const char *prev = key_pool[i - 1];
        size_t plen = strlen(prev);
        int extend = rand() % 2;
        size_t len = extend ? plen + 1 : (plen > 0 ? plen - 1 : 0);
        char *s = rb_malloc(len + 1);
        assert(s);
        memcpy(s, prev, plen < len ? plen : len);
        if (extend)
            s[plen] = mixed_charset[rand() % (int)(sizeof mixed_charset - 1)];
        s[len] = '\0';
        return s;
    }

    int len = rand_len();
    char *s = rb_malloc((size_t)len + 1);
    assert(s);
    if (category == KEY_REPEATED_CHAR) {
        char c = mixed_charset[rand() % (int)(sizeof mixed_charset - 1)];
        memset(s, c, (size_t)len);
    } else if (category == KEY_RAW_BYTES) {
        /* invariant: s[0..k) already filled with a non-NUL byte. */
        for (int k = 0; k < len; k++)
            s[k] = (char)(1 + rand() % 255);
    } else {
        const char *pool = (category == KEY_SPECIAL_CHARS)
            ? special_charset : charset;
        int pool_len = (category == KEY_SPECIAL_CHARS)
            ? (int)(sizeof special_charset - 1) : (int)(sizeof charset - 1);
        /* invariant: s[0..k) already filled with bytes from pool. */
        for (int k = 0; k < len; k++)
            s[k] = pool[rand() % pool_len];
    }
    s[len] = '\0';
    return s;
}

typedef struct {
    const char *key;  /* aliases into key_pool; shadow does not own it */
    int value;
} shadow_entry_t;

/* Sorted-by-key shadow array: the fuzz oracle. Capacity is KEYSPACE since
 * at most KEYSPACE distinct keys can ever be live at once. */
static shadow_entry_t shadow[KEYSPACE];
static int shadow_n;

/* Returns the index of key in shadow, or -1 if absent. */
static int shadow_find(const char *key)
{
    int lo = 0, hi = shadow_n - 1;
    /* invariant: if key is present, its index lies in [lo, hi]. */
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(shadow[mid].key, key);
        if (cmp == 0)
            return mid;
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* Inserts or overwrites key -> value, keeping shadow sorted by key. */
static void shadow_insert(const char *key, int value)
{
    int idx = shadow_find(key);
    if (idx >= 0) {
        shadow[idx].value = value;
        return;
    }
    int pos = 0;
    /* invariant: shadow[0..pos) all sort before key. */
    while (pos < shadow_n && strcmp(shadow[pos].key, key) < 0)
        pos++;
    assert(shadow_n < KEYSPACE);
    memmove(&shadow[pos + 1], &shadow[pos],
            (size_t)(shadow_n - pos) * sizeof *shadow);
    shadow[pos].key = key;
    shadow[pos].value = value;
    shadow_n++;
}

static void shadow_delete(const char *key)
{
    int idx = shadow_find(key);
    if (idx < 0)
        return;
    memmove(&shadow[idx], &shadow[idx + 1],
            (size_t)(shadow_n - idx - 1) * sizeof *shadow);
    shadow_n--;
}

typedef struct {
    const char *key;
    int value;
} foreach_entry_t;

static foreach_entry_t foreach_buf[KEYSPACE];
static int foreach_n;

static void collect_cb(const char *key, void *value, void *ctx)
{
    (void)ctx;
    assert(foreach_n < KEYSPACE);
    foreach_buf[foreach_n].key = key;
    foreach_buf[foreach_n].value = *(int *)value;
    foreach_n++;
}

/* Cross-checks rb_foreach's in-order output against the shadow oracle: same
 * count, same keys in the same (ascending) order, same values. */
static void check_foreach(const rbtree_t *t)
{
    foreach_n = 0;
    rb_foreach(t, collect_cb, NULL);
    assert(foreach_n == shadow_n);
    /* invariant: foreach_buf[0..i) already matched shadow[0..i) exactly. */
    for (int i = 0; i < shadow_n; i++) {
        assert(strcmp(foreach_buf[i].key, shadow[i].key) == 0);
        assert(foreach_buf[i].value == shadow[i].value);
    }
}

static void free_value(void *value)
{
    rb_free(value);
}

/* Re-checks rb_find against the oracle's current (unchanged) state; shared
 * by the find op and by the insert op's -1 path. */
static void assert_find_matches_shadow(const rbtree_t *t, const char *key)
{
    int idx = shadow_find(key);
    void *got = rb_find(t, key);
    if (idx < 0)
        assert(got == NULL);
    else
        assert(got != NULL && *(int *)got == shadow[idx].value);
}

int main(int argc, char *argv[])
{
    long ops = argc > 1 ? strtol(argv[1], NULL, 10) : 100000;
    unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 12345u;
    srand(seed);

    /* invariant: key_pool[0..i) already holds a live rb_malloc'd string. */
    for (int i = 0; i < KEYSPACE; i++)
        key_pool[i] = make_pool_key(i);

    rbtree_t *t = rb_create(free_value);
    assert(t);
    assert(rb_validate(t) == 0);
    check_foreach(t);

    /* invariant: every VALIDATE_INTERVAL-th op (and the first/last), t's
     * red-black invariants, size, and in-order traversal agree with the
     * shadow oracle. */
    for (long i = 0; i < ops; i++) {
        const char *key = key_pool[rand() % KEYSPACE];

        switch (rand() % 3) {
        case 0: { /* insert */
            int *value = rb_malloc(sizeof *value);
            assert(value);
            *value = (int)i;
            int rc = rb_insert(t, key, value);
            if (rc == 0) {
                assert(rb_find(t, key) == value);
                shadow_insert(key, *value);
            } else {
                /* Documented contract: on failure the tree is unchanged and
                 * value is NOT consumed, so the fuzzer -- not the tree --
                 * still owns and must free it, and the oracle must not
                 * move. */
                assert(rc == -1);
                rb_free(value);
                assert_find_matches_shadow(t, key);
            }
            break;
        }
        case 1: /* find */
            assert_find_matches_shadow(t, key);
            break;
        default: { /* delete */
            int idx = shadow_find(key);
            assert(rb_delete(t, key) == (idx < 0 ? -1 : 0));
            if (idx >= 0)
                shadow_delete(key);
            break;
        }
        }

        assert(rb_size(t) == (size_t)shadow_n);
        if (i % VALIDATE_INTERVAL == 0) {
            assert(rb_validate(t) == 0);
            check_foreach(t);
        }
    }

    assert(rb_validate(t) == 0);
    check_foreach(t);
    rb_destroy(t);

    for (int i = 0; i < KEYSPACE; i++)
        rb_free(key_pool[i]);

    printf("ok - fuzz: %ld ops, seed %u\n", ops, seed);
    return 0;
}
