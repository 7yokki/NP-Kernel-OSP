#ifndef NPK_MEMORY_H
#define NPK_MEMORY_H

#include "types.h"

void pmm_init(void);
paddr_t pmm_alloc_page(void);
void pmm_free_page(paddr_t page);
void pmm_retain_page(paddr_t page);
uint32_t pmm_page_refs(paddr_t page);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
void vmm_init(void);
paddr_t vmm_current_root(void);
paddr_t vmm_create_address_space(void);
void vmm_destroy_address_space(paddr_t root);
void vmm_map_identity(vaddr_t start, vaddr_t end, uint64_t flags);
bool vmm_map_page(vaddr_t virtual_address, paddr_t physical_address, uint64_t flags);
bool vmm_map_page_root(paddr_t root, vaddr_t virtual_address, paddr_t physical_address, uint64_t flags);
bool vmm_protect_page_root(paddr_t root, vaddr_t virtual_address, uint64_t flags);
paddr_t vmm_unmap_page_root(paddr_t root, vaddr_t virtual_address);
void vmm_unmap(vaddr_t virtual_address);
bool vmm_is_user_range(vaddr_t address, uint64_t length);
paddr_t vmm_lookup_root(paddr_t root, vaddr_t virtual_address, uint64_t required_flags);
paddr_t vmm_clone_user_space(paddr_t parent_root);
bool vmm_resolve_cow_fault(paddr_t root, vaddr_t virtual_address);
bool vmm_copyin(void *kernel_destination, vaddr_t user_source, size_t length);
bool vmm_copyout(vaddr_t user_destination, const void *kernel_source, size_t length);
void *phys_to_virt(paddr_t physical);
paddr_t virt_to_phys(const void *virtual_address);

#define VM_PRESENT (1ULL << 0)
#define VM_WRITE   (1ULL << 1)
#define VM_USER    (1ULL << 2)
#define VM_NX      (1ULL << 63)
#define VM_COW     (1ULL << 9) /* software COW marker in available PTE bits */
#define VM_SHARED  (1ULL << 10) /* software marker for shared writable user pages */
#define VM_DEVICE  (1ULL << 11) /* software marker for user-mapped device memory */

#endif
