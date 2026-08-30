#ifndef NPK_BOOT_H
#define NPK_BOOT_H

#include <limine.h>
#include "types.h"

typedef struct {
    struct limine_framebuffer *framebuffer;
    struct limine_memmap_response *memmap;
    struct limine_hhdm_response *hhdm;
    struct limine_executable_cmdline_response *cmdline;
    struct limine_module_response *modules;
    struct LIMINE_MP(response) *mp;
    uint64_t rsdp;
    bool rsdp_is_virtual;
} boot_info_t;

extern boot_info_t g_boot_info;
void boot_collect_info(void);

#endif
