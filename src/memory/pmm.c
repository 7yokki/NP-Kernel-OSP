#include <npk/boot.h>
#include <npk/log.h>
#include <npk/memory.h>

#define MAX_TRACKED_PAGES (1ULL << 20) /* 4 GiB physical address coverage. */
#define BITMAP_WORDS (MAX_TRACKED_PAGES / 64)
#define EARLY_RESERVED_END 0x01000000ULL

static uint64_t page_bitmap[BITMAP_WORDS];
static uint32_t page_refs[MAX_TRACKED_PAGES];
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t hhdm_offset;

static void set_used(uint64_t page) { page_bitmap[page / 64] |= 1ULL << (page % 64); }
static void set_free(uint64_t page) { page_bitmap[page / 64] &= ~(1ULL << (page % 64)); }
static bool is_used(uint64_t page) { return (page_bitmap[page / 64] >> (page % 64)) & 1U; }

void pmm_init(void) {
    for (size_t i = 0; i < BITMAP_WORDS; ++i) page_bitmap[i] = UINT64_MAX;
    for (size_t i = 0; i < MAX_TRACKED_PAGES; ++i) page_refs[i] = 0;
    total_pages = 0;
    free_pages = 0;
    hhdm_offset = g_boot_info.hhdm ? g_boot_info.hhdm->offset : 0;

    if (g_boot_info.memmap == NULL) {
        LOG_WARNF("pmm", "no memory map; allocator disabled", 0);
        return;
    }
    for (uint64_t i = 0; i < g_boot_info.memmap->entry_count; ++i) {
        struct limine_memmap_entry *entry = g_boot_info.memmap->entries[i];
        uint64_t first = entry->base / NPK_PAGE_SIZE;
        uint64_t last = (entry->base + entry->length) / NPK_PAGE_SIZE;
        if (last > MAX_TRACKED_PAGES) last = MAX_TRACKED_PAGES;
        if (entry->type != LIMINE_MEMMAP_USABLE || first >= last) continue;
        for (uint64_t page = first; page < last; ++page) {
            if (page * NPK_PAGE_SIZE < EARLY_RESERVED_END) continue;
            if (is_used(page)) { set_free(page); ++free_pages; }
        }
        total_pages += last - first;
    }
    LOG_INFOF("pmm", "free pages", free_pages);
    LOG_INFOF("pmm", "tracked pages", total_pages);
}

paddr_t pmm_alloc_page(void) {
    for (uint64_t word = 0; word < BITMAP_WORDS; ++word) {
        if (page_bitmap[word] == UINT64_MAX) continue;
        for (uint64_t bit = 0; bit < 64; ++bit) {
            uint64_t page = word * 64 + bit;
            if (page >= MAX_TRACKED_PAGES || is_used(page)) continue;
            set_used(page);
            page_refs[page] = 1;
            if (free_pages) --free_pages;
            return page * NPK_PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_retain_page(paddr_t physical) {
    uint64_t page = physical / NPK_PAGE_SIZE;
    if (physical % NPK_PAGE_SIZE != 0 || page >= MAX_TRACKED_PAGES || !is_used(page)) return;
    if (page_refs[page] != UINT32_MAX) ++page_refs[page];
}

uint32_t pmm_page_refs(paddr_t physical) {
    uint64_t page = physical / NPK_PAGE_SIZE;
    if (physical % NPK_PAGE_SIZE != 0 || page >= MAX_TRACKED_PAGES || !is_used(page)) return 0;
    return page_refs[page];
}

void pmm_free_page(paddr_t physical) {
    uint64_t page = physical / NPK_PAGE_SIZE;
    if (physical % NPK_PAGE_SIZE != 0 || page >= MAX_TRACKED_PAGES || !is_used(page) || page_refs[page] == 0) return;
    if (--page_refs[page] != 0) return;
    set_free(page);
    ++free_pages;
}

uint64_t pmm_total_pages(void) { return total_pages; }
uint64_t pmm_free_pages(void) { return free_pages; }
void *phys_to_virt(paddr_t physical) { return (void *)(physical + hhdm_offset); }
paddr_t virt_to_phys(const void *virtual_address) { return (paddr_t)virtual_address - hhdm_offset; }
