#ifndef NPK_SMP_H
#define NPK_SMP_H

#include "types.h"

void smp_init(void);
uint32_t smp_cpu_count(void);
uint32_t smp_online_count(void);
bool smp_enabled(void);

#endif

