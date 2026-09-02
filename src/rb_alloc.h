#ifndef RB_ALLOC_H
#define RB_ALLOC_H

#include <stddef.h>

void *rb_malloc(size_t size);
void rb_free(void *ptr);

#endif