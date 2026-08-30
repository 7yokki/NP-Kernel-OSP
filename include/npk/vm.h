#ifndef NPK_VM_H
#define NPK_VM_H

#include "types.h"

struct process;

enum {
    NPK_PROT_READ = 1,
    NPK_PROT_WRITE = 2,
    NPK_PROT_EXEC = 4,
};

enum {
    NPK_MAP_SHARED = 1,
    NPK_MAP_PRIVATE = 2,
    NPK_MAP_FIXED = 16,
    NPK_MAP_ANONYMOUS = 32,
    NPK_MAP_HEAP = 0x100,
    NPK_MAP_SHARED_OBJECT = 0x200,
    NPK_MAP_DEVICE = 0x400,
};

int vm_map_anonymous(struct process *process, vaddr_t hint, uint64_t length,
                     uint64_t protection, uint64_t flags, vaddr_t *result);
int vm_map_file(struct process *process, int handle, vaddr_t hint, uint64_t length,
                uint64_t protection, uint64_t flags, uint64_t offset, vaddr_t *result);
int vm_map_shared(struct process *process, int handle, vaddr_t hint, uint64_t length,
                  uint64_t protection, uint64_t flags, uint64_t offset, vaddr_t *result);
int vm_map_device(struct process *process, vaddr_t hint, uint64_t physical,
                  uint64_t length, uint64_t protection, uint64_t flags, vaddr_t *result);
int vm_reserve_range(struct process *process, vaddr_t start, vaddr_t length,
                      uint64_t protection, uint64_t flags);
int vm_reserve_heap(struct process *process, vaddr_t start, vaddr_t limit);
int vm_resize_heap(struct process *process, vaddr_t new_end);
int vm_unmap(struct process *process, vaddr_t address, uint64_t length);
void vm_revoke_device_mappings(struct process *process);
int vm_protect(struct process *process, vaddr_t address, uint64_t length, uint64_t protection);
void vm_destroy_all(struct process *process);
bool vm_clone_metadata(const struct process *parent, struct process *child);
bool vm_range_owned(struct process *process, vaddr_t address, uint64_t length, uint64_t protection);
/* Resolve a user not-present fault or a bounded downward stack-growth fault. */
bool vm_handle_page_fault(struct process *process, vaddr_t fault_address,
                          uint64_t error_code, vaddr_t user_rsp);
/* Used by kernel-mediated copyin/copyout to materialize lazy pages safely. */
bool vm_ensure_user_page(struct process *process, vaddr_t address, bool write);

#endif
