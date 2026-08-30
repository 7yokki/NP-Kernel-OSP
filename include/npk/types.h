#ifndef NPK_TYPES_H
#define NPK_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint64_t paddr_t;
typedef uint64_t vaddr_t;
typedef uint64_t usize_t;
typedef int64_t ssize_t;

#define NPK_PAGE_SIZE 4096ULL
#define NPK_MAX_CPUS 64U
#define NPK_MAX_PROCESSES 128U
#define NPK_MAX_THREADS 256U

#define NPK_PACKED __attribute__((packed))
#define NPK_ALIGNED(x) __attribute__((aligned(x)))
#define NPK_NORETURN __attribute__((noreturn))
#define NPK_UNUSED __attribute__((unused))

static inline void compiler_barrier(void) { __asm__ volatile("" ::: "memory"); }

#endif
