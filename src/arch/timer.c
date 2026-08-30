#include <npk/arch.h>
#include <npk/log.h>
#include <npk/timer.h>

#define PIT_INPUT_HZ 1193182U
#define PIT_COMMAND 0x43
#define PIT_CHANNEL0 0x40

static volatile uint64_t ticks;
static uint32_t frequency = 100;

void timer_init(uint32_t requested_frequency) {
    if (requested_frequency < 20 || requested_frequency > 1000) requested_frequency = 100;
    frequency = requested_frequency;
    uint32_t divisor = PIT_INPUT_HZ / frequency;
    if (divisor == 0 || divisor > 0xffff) divisor = 0xffff;
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xff));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
    LOG_INFOF("timer", "PIT frequency", frequency);
}

void timer_irq(void) { ++ticks; }
uint64_t timer_ticks(void) { return ticks; }
uint32_t timer_frequency(void) { return frequency; }
uint64_t timer_monotonic_ns(void) { return (ticks * 1000000000ULL) / frequency; }

void timer_sleep_ticks(uint64_t duration) {
    if (duration == 0) return;
    uint64_t deadline = ticks + duration;
    /* SYSCALL/exception entry runs with IF cleared. HLT must be executed
     * with IF set so the PIT can wake the CPU; restore the kernel invariant
     * after every wake. The segment layout used by SYSCALL keeps the IRET
     * return frame valid for this nested kernel interrupt. */
    while ((int64_t)(ticks - deadline) < 0)
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
}
