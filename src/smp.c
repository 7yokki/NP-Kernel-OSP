#include <limine.h>
#include <npk/arch.h>
#include <npk/boot.h>
#include <npk/log.h>
#include <npk/smp.h>

#define NPK_MAX_SMP_CPUS 16U
#define SMP_STATE_MAGIC 0x534d5043U
#define SMP_STATE_FAILED 0xffffffffU

typedef struct {
    uint32_t magic;
    uint32_t cpu_index;
    uint32_t processor_id;
    uint32_t lapic_id;
    volatile uint32_t online;
    volatile uint32_t arch_ready;
} smp_cpu_state_t;

static smp_cpu_state_t cpu_states[NPK_MAX_SMP_CPUS] __attribute__((aligned(64)));
static volatile uint32_t discovered_cpus = 1;
static volatile uint32_t online_cpus = 1;
static bool enabled;

static void smp_ap_entry(struct LIMINE_MP(info) *info) {
    smp_cpu_state_t *state = info ? (smp_cpu_state_t *)(uintptr_t)info->extra_argument : NULL;
    if (!state || state->magic != SMP_STATE_MAGIC || state->cpu_index >= NPK_MAX_SMP_CPUS) {
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    /* Limine enters the callback on the AP. Do not touch shared scheduler,
     * VFS, console, or process state before this CPU owns valid architectural
     * state. Each AP needs its own GDT/TSS/IST/GS and SYSCALL MSRs; IDT data
     * is shared, but IDTR itself is CPU-local. */
    __asm__ volatile("cli" ::: "memory");
    arch_init_cpu(state->cpu_index);
    __atomic_store_n(&state->arch_ready, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&state->online, 1U, __ATOMIC_RELEASE);
    __atomic_fetch_add(&online_cpus, 1U, __ATOMIC_SEQ_CST);

    /* The scheduler is intentionally BSP-only until per-CPU run queues,
     * interrupt routing, and cross-CPU rescheduling/IPI coordination exist.
     * An initialized AP must therefore remain quiescent instead of executing
     * code that assumes the BSP's global current/TSS/process pointers. */
    for (;;) __asm__ volatile("hlt" ::: "memory");
}

void smp_init(void) {
    enabled = false;
    discovered_cpus = 1;
    online_cpus = 1;
    for (unsigned i = 0; i < NPK_MAX_SMP_CPUS; ++i)
        cpu_states[i] = (smp_cpu_state_t){0};

    if (!g_boot_info.mp || !g_boot_info.mp->cpus || g_boot_info.mp->cpu_count == 0) {
        log_message(LOG_WARN, "smp", "Limine MP response unavailable; BSP-only mode");
        return;
    }

    uint64_t total = g_boot_info.mp->cpu_count;
    if (total > NPK_MAX_SMP_CPUS) total = NPK_MAX_SMP_CPUS;
    discovered_cpus = (uint32_t)total;
    for (uint32_t i = 0; i < discovered_cpus; ++i) {
        struct LIMINE_MP(info) *info = g_boot_info.mp->cpus[i];
        if (!info) continue;
        cpu_states[i].magic = SMP_STATE_MAGIC;
        cpu_states[i].cpu_index = i;
        cpu_states[i].processor_id = info->processor_id;
        cpu_states[i].lapic_id = info->lapic_id;
        if (info->lapic_id == g_boot_info.mp->bsp_lapic_id) {
            cpu_states[i].arch_ready = 1;
            cpu_states[i].online = 1;
            continue;
        }
        info->extra_argument = (uint64_t)(uintptr_t)&cpu_states[i];
        info->goto_address = smp_ap_entry;
    }

    enabled = discovered_cpus > 1;
    for (volatile uint64_t spin = 0;
         spin < 2000000 && smp_online_count() < discovered_cpus; ++spin)
        __asm__ volatile("pause" ::: "memory");

    LOG_INFOF("smp", "Limine CPUs discovered", discovered_cpus);
    LOG_INFOF("smp", "BSP/AP bring-up requested", enabled ? discovered_cpus - 1 : 0);
    LOG_INFOF("smp", "CPUs online", smp_online_count());
    if (smp_online_count() != discovered_cpus)
        log_message(LOG_WARN, "smp", "not all APs completed per-CPU arch init");
}

uint32_t smp_cpu_count(void) { return discovered_cpus; }
uint32_t smp_online_count(void) { return __atomic_load_n(&online_cpus, __ATOMIC_SEQ_CST); }
bool smp_enabled(void) { return enabled; }
