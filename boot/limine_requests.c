#include <limine.h>
#include <npk/boot.h>

LIMINE_BASE_REVISION(3)

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST,
    .revision = 0,
};

#if LIMINE_API_REVISION >= 1
#define NPK_LIMINE_MP_REQUEST_ID LIMINE_MP_REQUEST
#else
#define NPK_LIMINE_MP_REQUEST_ID LIMINE_SMP_REQUEST
#endif

__attribute__((used, section(".limine_requests")))
static volatile struct LIMINE_MP(request) mp_request = {
    .id = NPK_LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .flags = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start_marker[] = {
    0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf,
    0x785c6ed015d3e316, 0x181e920a7852b9d9,
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end_marker[] = {
    0xadc0e0531bb10d03, 0x9572709f31764c62,
};

boot_info_t g_boot_info;

void boot_collect_info(void) {
    g_boot_info.framebuffer = NULL;
    g_boot_info.memmap = NULL;
    g_boot_info.hhdm = NULL;
    g_boot_info.cmdline = NULL;
    g_boot_info.modules = NULL;
    g_boot_info.mp = NULL;
    g_boot_info.rsdp = 0;
    g_boot_info.rsdp_is_virtual = false;
    if (framebuffer_request.response != NULL && framebuffer_request.response->framebuffer_count != 0)
        g_boot_info.framebuffer = framebuffer_request.response->framebuffers[0];
    if (memmap_request.response != NULL)
        g_boot_info.memmap = memmap_request.response;
    if (hhdm_request.response != NULL)
        g_boot_info.hhdm = hhdm_request.response;
    if (cmdline_request.response != NULL)
        g_boot_info.cmdline = cmdline_request.response;
    if (module_request.response != NULL)
        g_boot_info.modules = module_request.response;
    if (mp_request.response != NULL)
        g_boot_info.mp = mp_request.response;
    if (rsdp_request.response != NULL) {
        g_boot_info.rsdp = (uint64_t)rsdp_request.response->address;
#if LIMINE_API_REVISION >= 1
        g_boot_info.rsdp_is_virtual = false;
#else
        g_boot_info.rsdp_is_virtual = true;
#endif
    }
}
