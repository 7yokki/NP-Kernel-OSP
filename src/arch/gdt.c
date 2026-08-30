#include <npk/arch.h>
#include <npk/log.h>
#include <npk/string.h>

#define NPK_ARCH_MAX_CPUS 16U

struct gdtr { uint16_t limit; uint64_t base; } __attribute__((packed));

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint32_t reserved2;
    uint32_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* The entry stubs use GS:[0] for the current kernel stack top and GS:[8]
 * for the saved user RSP. Keep this layout stable: it is an ABI between C,
 * the SYSCALL path, and every CPU-local instance. */
typedef struct {
    uint64_t kernel_stack_top;
    uint64_t user_rsp;
} npk_cpu_local_t;

static uint64_t gdts[NPK_ARCH_MAX_CPUS][7] __attribute__((aligned(16)));
static struct tss64 tsses[NPK_ARCH_MAX_CPUS] __attribute__((aligned(16)));
static npk_cpu_local_t cpu_locals[NPK_ARCH_MAX_CPUS] __attribute__((aligned(16)));
static uint8_t boot_stacks[NPK_ARCH_MAX_CPUS][16384] __attribute__((aligned(16)));
static uint8_t ist_stacks[NPK_ARCH_MAX_CPUS][32768] __attribute__((aligned(16)));

static uint32_t bounded_cpu_index(uint32_t cpu_index) {
    return cpu_index < NPK_ARCH_MAX_CPUS ? cpu_index : 0;
}

static void set_tss_descriptor(uint64_t *gdt, uint64_t base, uint32_t limit) {
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xffffU);
    low |= (base & 0xffffffULL) << 16;
    low |= 0x89ULL << 40; /* present, DPL0, available 64-bit TSS */
    low |= (uint64_t)((limit >> 16) & 0x0fU) << 48;
    low |= ((base >> 24) & 0xffULL) << 56;
    gdt[5] = low;
    gdt[6] = base >> 32;
}

static void load_cpu_gdt(uint32_t cpu_index) {
    uint64_t *gdt = gdts[cpu_index];
    struct gdtr descriptor = { .limit = sizeof(gdts[cpu_index]) - 1, .base = (uint64_t)gdt };
    __asm__ volatile(
        "lgdt %0\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "pushq $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov $0x28, %%ax\n"
        "ltr %%ax\n"
        : : "m"(descriptor) : "rax", "memory");
}

void gdt_init_cpu(uint32_t cpu_index) {
    cpu_index = bounded_cpu_index(cpu_index);
    uint64_t *gdt = gdts[cpu_index];
    struct tss64 *tss = &tsses[cpu_index];
    npk_cpu_local_t *local = &cpu_locals[cpu_index];

    memset(gdt, 0, sizeof(gdts[cpu_index]));
    memset(tss, 0, sizeof(*tss));
    memset(local, 0, sizeof(*local));

    tss->rsp0 = ((uint64_t)boot_stacks[cpu_index] + sizeof(boot_stacks[cpu_index])) & ~0xFULL;
    local->kernel_stack_top = tss->rsp0;
    tss->ist1 = ((uint64_t)ist_stacks[cpu_index] + sizeof(ist_stacks[cpu_index])) & ~0xFULL;
    tss->iomap_base = sizeof(*tss);

    gdt[1] = 0x00af9a000000ffffULL; /* ring 0 code, selector 0x08 */
    gdt[2] = 0x00cf92000000ffffULL; /* ring 0 data, selector 0x10 */
    gdt[3] = 0x00cff2000000ffffULL; /* ring 3 data, selector 0x1b */
    gdt[4] = 0x00affa000000ffffULL; /* ring 3 code, selector 0x23 */
    set_tss_descriptor(gdt, (uint64_t)tss, sizeof(*tss) - 1);
    load_cpu_gdt(cpu_index);

    /* Kernel entry always starts with GS_BASE pointing at this CPU's local
     * area. KERNEL_GS_BASE is the user GS value selected by the current
     * thread and is swapped in only while executing ring-3 code. */
    wrmsr(NPK_IA32_GS_BASE, (uint64_t)local);
    wrmsr(NPK_IA32_KERNEL_GS_BASE, 0);
    arch_set_fs_base(0);
}

void tss_set_rsp0(uint64_t stack_top) {
    uint32_t cpu_index = arch_cpu_index();
    tsses[cpu_index].rsp0 = stack_top & ~0xFULL;
    cpu_locals[cpu_index].kernel_stack_top = tsses[cpu_index].rsp0;
}

uint32_t arch_cpu_index(void) {
    uint64_t gs = rdmsr(NPK_IA32_GS_BASE);
    for (uint32_t i = 0; i < NPK_ARCH_MAX_CPUS; ++i)
        if (gs == (uint64_t)&cpu_locals[i]) return i;
    return 0;
}

void arch_set_fs_base(uint64_t base) {
    wrmsr(NPK_IA32_FS_BASE, base);
}

uint64_t arch_get_fs_base(void) {
    return rdmsr(NPK_IA32_FS_BASE);
}

void arch_set_user_gs_base(uint64_t base) {
    wrmsr(NPK_IA32_KERNEL_GS_BASE, base);
}

uint64_t arch_get_user_gs_base(void) {
    return rdmsr(NPK_IA32_KERNEL_GS_BASE);
}

void gdt_init(void) {
    gdt_init_cpu(0);
    LOG_INFOF("gdt", "CPU-local descriptors including TSS", 1);
}

void arch_init_cpu(uint32_t cpu_index) {
    gdt_init_cpu(cpu_index);
    idt_load_current();
    syscall_cpu_init();
}

