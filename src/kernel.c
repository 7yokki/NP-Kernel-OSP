#include <npk/acpi.h>
#include <npk/arch.h>
#include <npk/block.h>
#include <npk/boot.h>
#include <npk/elf.h>
#include <npk/console.h>
#include <npk/heap.h>
#include <npk/keyboard.h>
#include <npk/log.h>
#include <npk/memory.h>
#include <npk/panic.h>
#include <npk/process.h>
#include <npk/string.h>
#include <npk/syscall.h>
#include <npk/smp.h>
#include <npk/vfs.h>
#include <npk/vm.h>

extern void vga_init(void);
extern uint64_t limine_base_revision[3];

#ifndef NPK_ENABLE_USER_DEMO
#define NPK_ENABLE_USER_DEMO 0
#endif

#if NPK_ENABLE_USER_DEMO
static void kernel_launch_user_demo(void) {
    int fd = vfs_open("/bin/hello.elf");
    ssize_t size = fd >= 0 ? vfs_size(fd) : -1;
    if (fd < 0 || size <= 0 || (uint64_t)size > 8 * 1024 * 1024) {
        if (fd >= 0) vfs_close(fd);
        log_message(LOG_ERROR, "user", "hello.elf unavailable for ring3 launch");
        return;
    }
    void *image = kmalloc((size_t)size);
    bool loaded = image && vfs_read(fd, image, (size_t)size) == size;
    vfs_close(fd);
    if (!loaded) {
        if (image) kfree(image);
        log_message(LOG_ERROR, "user", "mmap.elf image read failed");
        return;
    }
    process_t *process = process_exec_image(image, (size_t)size);
    kfree(image);
    if (!process) {
        log_message(LOG_ERROR, "user", "secure exec rejected hello.elf");
        return;
    }
    LOG_INFOF("user", "launching ring3 pid", process->pid);
    /* Keep PIT IRQ0 enabled so this also exercises preemptive scheduling. */
    process_launch_user(process);
}
#endif

#if 0
static void scheduler_smoke_thread(void *argument) {
    LOG_INFOF("sched", "kernel thread executed", (uint64_t)(uintptr_t)argument);
}
#endif

static void kernel_demo(void) {
    log_message(LOG_INFO, "demo", "kernel.c is in control; custom boot flow is active");
    log_message(LOG_INFO, "demo", "Türkçe klavye: ş ğ ü ö ç İ Ş Ğ Ü Ö Ç");
    paddr_t page = pmm_alloc_page();
    LOG_INFOF("demo", "one page allocated", page);
    if (page) pmm_free_page(page);
    log_message(LOG_INFO, "demo", "allocator page returned successfully");
    void *heap_a = kmalloc(37);
    void *heap_b = kcalloc(8, 24);
    bool heap_ok = heap_a != NULL && heap_b != NULL && kheap_validate();
    kfree(heap_a);
    kfree(heap_b);
    heap_ok = heap_ok && kheap_validate();
    if (heap_ok) log_message(LOG_INFO, "heap", "kmalloc/kcalloc/kfree integrity check passed");
    else log_message(LOG_ERROR, "heap", "kernel heap integrity check failed");
    uint8_t sector[ NPK_SECTOR_SIZE ];
    int ata_result = ata_read_sectors(0, 1, sector);
    LOG_INFOF("ata", "read sector 0 result", (uint64_t)(uint32_t)ata_result);
    process_t vm_test_process;
    memset(&vm_test_process, 0, sizeof(vm_test_process));
    vm_test_process.alive = 1;
    vm_test_process.address_space_root = vmm_create_address_space();
    vaddr_t mapped = 0;
    int map_result = vm_test_process.address_space_root ? vm_map_anonymous(&vm_test_process, 0, NPK_PAGE_SIZE * 2, NPK_PROT_READ | NPK_PROT_WRITE, NPK_MAP_PRIVATE | NPK_MAP_ANONYMOUS, &mapped) : -12;
    bool map_owned = map_result == 0 && vm_range_owned(&vm_test_process, mapped, NPK_PAGE_SIZE * 2, NPK_PROT_READ | NPK_PROT_WRITE);
    int unmap_result = map_result == 0 ? vm_unmap(&vm_test_process, mapped, NPK_PAGE_SIZE * 2) : map_result;
    if (vm_test_process.address_space_root) vmm_destroy_address_space(vm_test_process.address_space_root);
    LOG_INFOF("vm", "temporary address-space root", vm_test_process.address_space_root);
    LOG_INFOF("vm", "anonymous mmap result", (uint64_t)(uint32_t)map_result);
    LOG_INFOF("vm", "anonymous mmap ownership", map_owned ? 1 : 0);
    LOG_INFOF("vm", "anonymous munmap result", (uint64_t)(uint32_t)unmap_result);
    int elf_fd = vfs_open("/bin/hello.elf");
    ssize_t elf_size = elf_fd >= 0 ? vfs_size(elf_fd) : -1;
    bool elf_ok = false;
    if (elf_fd >= 0 && elf_size > 0 && (uint64_t)elf_size <= 8 * 1024 * 1024) {
        void *elf_image = kmalloc((size_t)elf_size);
        if (elf_image && vfs_read(elf_fd, elf_image, (size_t)elf_size) == elf_size) {
            paddr_t elf_root = vmm_create_address_space();
            elf_load_result_t elf_loaded;
            int elf_status = elf_root ? elf64_load_in_address_space(elf_image, (size_t)elf_size, elf_root, 0, &elf_loaded) : -12;
            elf_ok = elf_status == 0 && elf_loaded.entry == 0x0000000000400000ULL;
            if (elf_root) vmm_destroy_address_space(elf_root);
        }
        if (elf_image) kfree(elf_image);
    }
    if (elf_fd >= 0) vfs_close(elf_fd);
    LOG_INFOF("elf", "hello.elf validated, loaded, and rolled back", elf_ok ? 1 : 0);
    int proc_fd = vfs_open("/proc/meminfo");
    char proc_sample[128];
    ssize_t proc_bytes = proc_fd >= 0 ? vfs_read(proc_fd, proc_sample, sizeof(proc_sample)) : -1;
    LOG_INFOF("proc", "meminfo read bytes", proc_bytes > 0 ? (uint64_t)proc_bytes : 0);
    if (proc_fd >= 0) vfs_close(proc_fd);
#if 0
    thread_t *smoke_thread = scheduler_create_kernel_thread(scheduler_smoke_thread, (void *)0x5343484544ULL);
    LOG_INFOF("sched", "cooperative kernel thread created", smoke_thread ? smoke_thread->tid : 0);
#endif
    log_message(LOG_INFO, "demo", "ring0 kernel descriptors and ring3 user descriptors are installed");
    log_message(LOG_WARN, "demo", "full Linux userspace compatibility is intentionally staged; unsupported syscalls return -ENOSYS");
    int fd = vfs_open("/README.txt");
    if (fd >= 0) {
        char sample[96];
        ssize_t bytes = vfs_read(fd, sample, sizeof(sample) - 1);
        if (bytes > 0) {
            sample[bytes] = '\0';
            LOG_INFOF("vfs", "initramfs README.txt bytes", (uint64_t)bytes);
            console_write("[VFS] /README.txt: ");
            console_write(sample);
            console_write("\n");
        }
    } else {
        log_message(LOG_WARN, "vfs", "sample initramfs file is unavailable");
    }
    int root_fd = vfs_open("/");
    uint8_t directory_sample[256];
    ssize_t directory_bytes = root_fd >= 0 ? vfs_getdents64(root_fd, directory_sample, sizeof(directory_sample)) : -1;
    vfs_stat_t root_status;
    int stat_result = root_fd >= 0 ? vfs_stat_fd(root_fd, &root_status) : -9;
    LOG_INFOF("vfs", "root getdents64 bytes", directory_bytes > 0 ? (uint64_t)directory_bytes : 0);
    LOG_INFOF("vfs", "root fstat result", (uint64_t)(uint32_t)stat_result);
    if (root_fd >= 0) vfs_close(root_fd);
}

void kernel_main(void) {
    boot_collect_info();
    vga_init();
    console_init();
    log_init();
    console_clear();

    console_write("NPKernel / No Problem Kernel\n");
    console_write("x86_64 | C17 + NASM | Limine + UEFI/GOP\n\n");
    LOG_INFOF("boot", "Limine base revision", LIMINE_LOADED_BASE_REVISION);
    if (g_boot_info.framebuffer) {
        LOG_INFOF("gop", "framebuffer width", g_boot_info.framebuffer->width);
        LOG_INFOF("gop", "framebuffer height", g_boot_info.framebuffer->height);
    }
    if (g_boot_info.hhdm) LOG_INFOF("boot", "HHDM offset", g_boot_info.hhdm->offset);

    arch_init();
    smp_init();
    pmm_init();
    vmm_init();
    kheap_init();
    keyboard_init();
    ata_init();
    process_init();
    vfs_init();
    syscall_init();
    kernel_demo();

    log_message(LOG_INFO, "boot", "NPKernel initialization complete; interrupts enabled");
#if NPK_ENABLE_USER_DEMO
    kernel_launch_user_demo();
#endif
    __asm__ volatile("sti");
    for (;;) {
        __asm__ volatile("hlt");
        acpi_poll();
        scheduler_tick();
        if (scheduler_reschedule_pending()) scheduler_yield();
    }
}
