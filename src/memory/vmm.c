#include <npk/log.h>
#include <npk/memory.h>
#include <npk/process.h>
#include <npk/vm.h>
#include <npk/string.h>

#define ENTRY_ADDR_MASK 0x000ffffffffff000ULL
#define PAGE_TABLE_ENTRIES 512U
#define USER_LIMIT 0x0000800000000000ULL
#define KERNEL_PML4_FIRST 256U
#define VM_PAGE_SIZE 0x80ULL /* internal page-table PS bit, not exposed as a leaf flag */

static bool canonical(uint64_t address) {
    uint64_t upper = address >> 48;
    return upper == 0 || upper == 0xffff;
}

static uint64_t leaf_flags(uint64_t flags) {
    uint64_t value = VM_PRESENT;
    if (flags & VM_WRITE) value |= VM_WRITE;
    if (flags & VM_USER) value |= VM_USER;
    if (flags & VM_NX) value |= VM_NX;
    if (flags & VM_SHARED) value |= VM_SHARED;
    if (flags & VM_DEVICE) value |= VM_DEVICE;
    return value;
}

static uint64_t table_flags(uint64_t flags) {
    uint64_t value = VM_PRESENT | VM_WRITE;
    if (flags & VM_USER) value |= VM_USER;
    return value;
}

static uint64_t *table_from_entry(uint64_t entry) {
    return (uint64_t *)phys_to_virt(entry & ENTRY_ADDR_MASK);
}

static uint64_t read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ENTRY_ADDR_MASK;
}

static bool root_allowed(paddr_t root) {
    return root != 0 && (root & (NPK_PAGE_SIZE - 1)) == 0;
}

static uint64_t *leaf_entry_ptr(paddr_t root, vaddr_t virtual_address) {
    if (!root_allowed(root) || !canonical(virtual_address)) return NULL;
    uint64_t *pml4 = (uint64_t *)phys_to_virt(root);
    uint64_t entry = pml4[(virtual_address >> 39) & 0x1ff];
    if (!(entry & VM_PRESENT) || (entry & VM_PAGE_SIZE)) return NULL;
    uint64_t *pdpt = table_from_entry(entry);
    entry = pdpt[(virtual_address >> 30) & 0x1ff];
    if (!(entry & VM_PRESENT) || (entry & VM_PAGE_SIZE)) return NULL;
    uint64_t *pd = table_from_entry(entry);
    entry = pd[(virtual_address >> 21) & 0x1ff];
    if (!(entry & VM_PRESENT) || (entry & VM_PAGE_SIZE)) return NULL;
    uint64_t *pt = table_from_entry(entry);
    return &pt[(virtual_address >> 12) & 0x1ff];
}

static bool clone_user_table(uint64_t *source, uint64_t *destination,
                             unsigned level, vaddr_t base) {
    bool found = false;
    for (unsigned index = 0; index < PAGE_TABLE_ENTRIES; ++index) {
        uint64_t entry = source[index];
        if (!(entry & VM_PRESENT)) continue;
        if (level == 1) {
            if (!(entry & VM_USER) || (entry & VM_DEVICE)) continue;
            destination[index] = entry;
            pmm_retain_page(entry & ENTRY_ADDR_MASK);
            found = true;
            continue;
        }
        if (entry & VM_PAGE_SIZE) continue;
        paddr_t child_physical = pmm_alloc_page();
        if (!child_physical) return false;
        uint64_t *child = (uint64_t *)phys_to_virt(child_physical);
        memset(child, 0, NPK_PAGE_SIZE);
        vaddr_t child_base = base + ((vaddr_t)index << ((level - 1) * 9 + 12));
        bool child_found = clone_user_table(table_from_entry(entry), child,
                                            level - 1, child_base);
        if (!child_found) {
            pmm_free_page(child_physical);
            continue;
        }
        destination[index] = child_physical | VM_PRESENT | (entry & (VM_WRITE | VM_USER));
        found = true;
    }
    return found;
}

static void mark_cow_table(uint64_t *parent, uint64_t *child, unsigned level) {
    for (unsigned index = 0; index < PAGE_TABLE_ENTRIES; ++index) {
        uint64_t parent_entry = parent[index];
        uint64_t child_entry = child[index];
        if (!(parent_entry & VM_PRESENT) || !(child_entry & VM_PRESENT)) continue;
        if (level == 1) {
            if (!(parent_entry & VM_USER)) continue;
            if ((parent_entry & VM_SHARED) != 0) continue;
            if (parent_entry & VM_WRITE) {
                parent_entry = (parent_entry & ~VM_WRITE) | VM_COW;
                child_entry = (child_entry & ~VM_WRITE) | VM_COW;
                parent[index] = parent_entry;
                child[index] = child_entry;
            }
            continue;
        }
        if (parent_entry & VM_PAGE_SIZE || child_entry & VM_PAGE_SIZE) continue;
        mark_cow_table(table_from_entry(parent_entry), table_from_entry(child_entry), level - 1);
    }
}

static uint64_t *ensure_table(uint64_t *parent, uint16_t index, uint64_t flags) {
    uint64_t entry = parent[index];
    if (entry & VM_PAGE_SIZE) return NULL;
    if (entry & VM_PRESENT) {
        /* Existing intermediate entries retain their original permissions.
         * User mappings are created in the private low-half of a new root;
         * widening a supervisor branch here would make the whole subtree
         * user-accessible and violate the kernel/user boundary. */
        return table_from_entry(entry);
    }
    paddr_t page = pmm_alloc_page();
    if (page == 0) return NULL;
    uint64_t *table = (uint64_t *)phys_to_virt(page);
    memset(table, 0, NPK_PAGE_SIZE);
    parent[index] = page | table_flags(flags);
    return table;
}

bool vmm_map_page_root(paddr_t root, vaddr_t virtual_address, paddr_t physical_address, uint64_t flags) {
    if (!root_allowed(root) || (virtual_address & (NPK_PAGE_SIZE - 1)) != 0 ||
        (physical_address & (NPK_PAGE_SIZE - 1)) != 0 || !canonical(virtual_address)) return false;
    if ((flags & VM_USER) && (virtual_address >= USER_LIMIT || virtual_address > USER_LIMIT - NPK_PAGE_SIZE)) return false;
    if (physical_address & ~ENTRY_ADDR_MASK) return false;

    uint64_t *pml4 = (uint64_t *)phys_to_virt(root);
    uint16_t i4 = (virtual_address >> 39) & 0x1ff;
    uint16_t i3 = (virtual_address >> 30) & 0x1ff;
    uint16_t i2 = (virtual_address >> 21) & 0x1ff;
    uint16_t i1 = (virtual_address >> 12) & 0x1ff;
    uint64_t *pdpt = ensure_table(pml4, i4, flags);
    if (!pdpt) return false;
    uint64_t *pd = ensure_table(pdpt, i3, flags);
    if (!pd) return false;
    uint64_t *pt = ensure_table(pd, i2, flags);
    if (!pt || (pt[i1] & VM_PRESENT)) return false;
    pt[i1] = physical_address | leaf_flags(flags);
    if (root == read_cr3()) __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return true;
}

bool vmm_map_page(vaddr_t virtual_address, paddr_t physical_address, uint64_t flags) {
    return vmm_map_page_root(read_cr3(), virtual_address, physical_address, flags);
}

bool vmm_protect_page_root(paddr_t root, vaddr_t virtual_address, uint64_t flags) {
    if (!root_allowed(root) || (virtual_address & (NPK_PAGE_SIZE - 1)) != 0 ||
        !canonical(virtual_address) || !(flags & VM_USER) ||
        (flags & ~(VM_USER | VM_WRITE | VM_NX)) != 0 || virtual_address >= USER_LIMIT)
        return false;
    uint64_t *pte = leaf_entry_ptr(root, virtual_address);
    if (!pte || !(*pte & VM_PRESENT) || !(*pte & VM_USER)) return false;
    uint64_t old = *pte;
    uint64_t updated = (old & ENTRY_ADDR_MASK) | leaf_flags(flags);
    updated |= old & (VM_SHARED | VM_DEVICE);
    if (old & VM_COW) {
        /* A COW page may not become writable merely because its VMA was
         * changed. The write fault must still split the shared page. */
        updated |= VM_COW;
        updated &= ~VM_WRITE;
    }
    *pte = updated;
    if (root == read_cr3()) __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return true;
}

paddr_t vmm_current_root(void) { return read_cr3(); }

paddr_t vmm_clone_user_space(paddr_t parent_root) {
    if (!root_allowed(parent_root)) return 0;
    paddr_t child_root = vmm_create_address_space();
    if (!child_root) return 0;
    uint64_t *parent = (uint64_t *)phys_to_virt(parent_root);
    uint64_t *child = (uint64_t *)phys_to_virt(child_root);
    if (clone_user_table(parent, child, 4, 0)) {
        mark_cow_table(parent, child, 4);
        if (parent_root == read_cr3()) {
            __asm__ volatile ("mov %%cr3, %%rax\n\t"
                              "mov %%rax, %%cr3" ::: "rax", "memory");
        }
        return child_root;
    }
    vmm_destroy_address_space(child_root);
    return 0;
}

bool vmm_resolve_cow_fault(paddr_t root, vaddr_t virtual_address) {
    uint64_t *pte = leaf_entry_ptr(root, virtual_address);
    if (!pte || !(*pte & VM_PRESENT) || !(*pte & VM_USER) || !(*pte & VM_COW)) return false;
    paddr_t old_page = *pte & ENTRY_ADDR_MASK;
    uint32_t references = pmm_page_refs(old_page);
    if (references == 0) return false;
    if (references == 1) {
        *pte = (*pte | VM_WRITE) & ~VM_COW;
    } else {
        paddr_t new_page = pmm_alloc_page();
        if (!new_page) return false;
        memcpy(phys_to_virt(new_page), phys_to_virt(old_page), NPK_PAGE_SIZE);
        uint64_t new_entry = (*pte & ~ENTRY_ADDR_MASK) | new_page;
        new_entry = (new_entry | VM_WRITE) & ~VM_COW;
        *pte = new_entry;
        pmm_free_page(old_page);
    }
    if (root == read_cr3()) __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return true;
}

paddr_t vmm_create_address_space(void) {
    paddr_t current = read_cr3();
    paddr_t root = pmm_alloc_page();
    if (root == 0) return 0;
    uint64_t *new_pml4 = (uint64_t *)phys_to_virt(root);
    uint64_t *kernel_pml4 = (uint64_t *)phys_to_virt(current);
    memset(new_pml4, 0, NPK_PAGE_SIZE);
    for (uint16_t i = KERNEL_PML4_FIRST; i < PAGE_TABLE_ENTRIES; ++i) new_pml4[i] = kernel_pml4[i];
    return root;
}

static void destroy_table(uint64_t *table, unsigned level) {
    for (unsigned i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
        uint64_t entry = table[i];
        if (!(entry & VM_PRESENT)) continue;
        if (entry & VM_PAGE_SIZE) {
            pmm_free_page(entry & ENTRY_ADDR_MASK);
            table[i] = 0;
            continue;
        }
        if (level > 1) {
            destroy_table(table_from_entry(entry), level - 1);
            pmm_free_page(entry & ENTRY_ADDR_MASK);
        } else {
            if ((entry & VM_DEVICE) == 0) pmm_free_page(entry & ENTRY_ADDR_MASK);
        }
        table[i] = 0;
    }
}

void vmm_destroy_address_space(paddr_t root) {
    if (!root_allowed(root) || root == read_cr3()) return;
    uint64_t *pml4 = (uint64_t *)phys_to_virt(root);
    for (unsigned i = 0; i < KERNEL_PML4_FIRST; ++i) {
        if (!(pml4[i] & VM_PRESENT)) continue;
        destroy_table(table_from_entry(pml4[i]), 3);
        pmm_free_page(pml4[i] & ENTRY_ADDR_MASK);
    }
    pmm_free_page(root);
}

paddr_t vmm_unmap_page_root(paddr_t root, vaddr_t virtual_address) {
    if (!root_allowed(root) || !canonical(virtual_address)) return 0;
    uint64_t *pml4 = (uint64_t *)phys_to_virt(root);
    uint64_t e = pml4[(virtual_address >> 39) & 0x1ff]; if (!(e & VM_PRESENT) || (e & VM_PAGE_SIZE)) return 0;
    uint64_t *pdpt = table_from_entry(e);
    e = pdpt[(virtual_address >> 30) & 0x1ff]; if (!(e & VM_PRESENT) || (e & VM_PAGE_SIZE)) return 0;
    uint64_t *pd = table_from_entry(e);
    e = pd[(virtual_address >> 21) & 0x1ff]; if (!(e & VM_PRESENT) || (e & VM_PAGE_SIZE)) return 0;
    uint64_t *pt = table_from_entry(e);
    uint16_t index = (virtual_address >> 12) & 0x1ff;
    paddr_t physical = pt[index] & ENTRY_ADDR_MASK;
    pt[index] = 0;
    if (root == read_cr3()) __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return physical;
}

void vmm_unmap(vaddr_t virtual_address) {
    (void)vmm_unmap_page_root(read_cr3(), virtual_address);
}

paddr_t vmm_lookup_root(paddr_t root, vaddr_t virtual_address, uint64_t required_flags) {
    if (!root_allowed(root) || !canonical(virtual_address)) return 0;
    uint64_t *pml4 = (uint64_t *)phys_to_virt(root);
    uint64_t e = pml4[(virtual_address >> 39) & 0x1ff]; if (!(e & VM_PRESENT) || (e & VM_PAGE_SIZE)) return 0;
    uint64_t *pdpt = table_from_entry(e);
    e = pdpt[(virtual_address >> 30) & 0x1ff]; if (!(e & VM_PRESENT) || (e & VM_PAGE_SIZE)) return 0;
    uint64_t *pd = table_from_entry(e);
    e = pd[(virtual_address >> 21) & 0x1ff]; if (!(e & VM_PRESENT) || (e & VM_PAGE_SIZE)) return 0;
    uint64_t *pt = table_from_entry(e);
    e = pt[(virtual_address >> 12) & 0x1ff];
    if (!(e & VM_PRESENT) || (required_flags & VM_USER && !(e & VM_USER)) ||
        (required_flags & VM_WRITE && !(e & VM_WRITE)) || (required_flags & VM_NX && !(e & VM_NX))) return 0;
    return (e & ENTRY_ADDR_MASK) | (virtual_address & (NPK_PAGE_SIZE - 1));
}

bool vmm_is_user_range(vaddr_t address, uint64_t length) {
    if (length == 0) return address < USER_LIMIT;
    return address < USER_LIMIT && length <= USER_LIMIT - address &&
           canonical(address) && canonical(address + length - 1);
}

void vmm_init(void) {
    LOG_INFOF("vmm", "active pml4", read_cr3());
    LOG_INFOF("vmm", "user address limit", USER_LIMIT);
}

void vmm_map_identity(vaddr_t start, vaddr_t end, uint64_t flags) {
    if (start > end || end == 0) return;
    start &= ~(NPK_PAGE_SIZE - 1);
    if (end > UINT64_MAX - (NPK_PAGE_SIZE - 1)) return;
    end = (end + NPK_PAGE_SIZE - 1) & ~(NPK_PAGE_SIZE - 1);
    for (vaddr_t address = start; address < end; address += NPK_PAGE_SIZE)
        if (!vmm_map_page(address, address, flags)) { LOG_ERRORF("vmm", "page map failed", address); return; }
}


bool vmm_copyin(void *kernel_destination, vaddr_t user_source, size_t length) {
    if (kernel_destination == NULL || !vmm_is_user_range(user_source, length)) return false;
    uint8_t *destination = (uint8_t *)kernel_destination;
    uint64_t copied = 0;
    while (copied < length) {
        vaddr_t user_address = user_source + copied;
        paddr_t physical = vmm_lookup_root(read_cr3(), user_address, VM_USER);
        if (physical == 0) {
            process_t *process = process_current();
            if (!vm_ensure_user_page(process, user_address, false)) return false;
            physical = vmm_lookup_root(read_cr3(), user_address, VM_USER);
        }
        if (physical == 0) return false;
        uint64_t page_left = NPK_PAGE_SIZE - (user_address & (NPK_PAGE_SIZE - 1));
        uint64_t amount = length - copied < page_left ? length - copied : page_left;
        memcpy(destination + copied, phys_to_virt(physical), (size_t)amount);
        copied += amount;
    }
    return true;
}

bool vmm_copyout(vaddr_t user_destination, const void *kernel_source, size_t length) {
    if (kernel_source == NULL || !vmm_is_user_range(user_destination, length)) return false;
    const uint8_t *source = (const uint8_t *)kernel_source;
    uint64_t copied = 0;
    while (copied < length) {
        vaddr_t user_address = user_destination + copied;
        paddr_t physical = vmm_lookup_root(read_cr3(), user_address, VM_USER | VM_WRITE);
        if (physical == 0) {
            process_t *process = process_current();
            if (!vm_ensure_user_page(process, user_address, true)) return false;
            physical = vmm_lookup_root(read_cr3(), user_address, VM_USER | VM_WRITE);
        }
        if (physical == 0) return false;
        uint64_t page_left = NPK_PAGE_SIZE - (user_address & (NPK_PAGE_SIZE - 1));
        uint64_t amount = length - copied < page_left ? length - copied : page_left;
        memcpy(phys_to_virt(physical), source + copied, (size_t)amount);
        copied += amount;
    }
    return true;
}
