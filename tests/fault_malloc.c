#include "../src/rb_alloc.h"
#include <stdlib.h>

/* Definitions of rb_malloc/rb_free for the test binary. The Makefile links
 * this file instead of src/rb_alloc.c into $(BIN), so these are the only
 * rb_malloc/rb_free in that binary -- lets tests force a specific rb_malloc
 * call to fail without touching libc malloc or the production wrappers. */

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