#ifndef NPK_TIMER_H
#define NPK_TIMER_H

#include "types.h"

void timer_init(uint32_t frequency_hz);
void timer_irq(void);
uint64_t timer_ticks(void);
uint32_t timer_frequency(void);
uint64_t timer_monotonic_ns(void);
void timer_sleep_ticks(uint64_t ticks);

#endif
