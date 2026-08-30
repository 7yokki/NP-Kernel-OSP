#ifndef NPK_PANIC_H
#define NPK_PANIC_H

#include "types.h"

NPK_NORETURN void panic(const char *reason);
NPK_NORETURN void panic_with_code(const char *reason, uint64_t code);

#endif
