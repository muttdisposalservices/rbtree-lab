#include <stdlib.h>

/* Strong overrides of the weak rb_malloc/rb_free in src/rbtree.c (see
 * CLAUDE.md's allocation-routing rule). Lets tests force a specific
 * rb_malloc call to fail without touching libc malloc or the src build. */

static int fault_armed = 0;
static int fault_target = 0;
static int fault_count = 0;
static int free_count = 0;

void fault_malloc_arm(int nth_call_to_fail)
{
    fault_target = nth_call_to_fail;
    fault_count = 0;
    free_count = 0;
    fault_armed = 1;
}

void fault_malloc_disarm(void)
{
    fault_armed = 0;
}

int fault_malloc_free_count(void)
{
    return free_count;
}

void *rb_malloc(size_t size)
{
    if (fault_armed) {
        fault_count++;
        if (fault_count == fault_target) {
            fault_armed = 0;
            return NULL;
        }
    }
    return malloc(size);
}

void rb_free(void *ptr)
{
    free_count++;
    free(ptr);
}