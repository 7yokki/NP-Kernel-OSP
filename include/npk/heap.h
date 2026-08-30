#ifndef NPK_HEAP_H
#define NPK_HEAP_H

#include "types.h"

#define NPK_HEAP_BASE 0xffff900000000000ULL
#define NPK_HEAP_LIMIT 0xffff900004000000ULL /* 64 MiB virtual heap window. */
#define NPK_HEAP_ALIGNMENT 16U

void kheap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void kfree(void *pointer);
bool kheap_validate(void);
uint64_t kheap_mapped_bytes(void);
uint64_t kheap_free_bytes(void);

#endif
