#include <npk/heap.h>
#include <npk/memory.h>
#include <npk/process.h>
#include <npk/string.h>
#include <npk/vfs.h>
#include <npk/vm.h>

#define VM_EINVAL -22
#define VM_ENOMEM -12
#define VM_EEXIST -17
#define VM_USER_MMAP_BASE 0x0000001000000000ULL
#define VM_USER_MMAP_LIMIT 0x0000400000000000ULL
#define NPK_USER_MIN 0x0000000000010000ULL
#define NPK_USER_MAX 0x0000800000000000ULL

typedef struct npk_vma {
    vaddr_t start;
    vaddr_t end;
    uint64_t protection;
    uint64_t flags;
    int backing_handle;
    uint64_t backing_offset;
    uint64_t backing_size;
    struct npk_vma *next;
} npk_vma_t;

static bool add_overflow(uint64_t a, uint64_t b) { return b > UINT64_MAX - a; }
static uint64_t page_align(uint64_t value) { return value & ~(NPK_PAGE_SIZE - 1); }
static bool page_align_up(uint64_t value, uint64_t *out) {
    if (value > UINT64_MAX - (NPK_PAGE_SIZE - 1)) return false;
    *out = (value + NPK_PAGE_SIZE - 1) & ~(NPK_PAGE_SIZE - 1);
    return true;
}

static bool overlaps(npk_vma_t *list, vaddr_t start, vaddr_t end) {
    for (npk_vma_t *vma = list; vma; vma = vma->next)
        if (start < vma->end && end > vma->start) return true;
    return false;
}

static uint64_t page_flags_for(uint64_t protection) {
    uint64_t flags = VM_USER;
    if (protection & NPK_PROT_WRITE) flags |= VM_WRITE;
    if (!(protection & NPK_PROT_EXEC)) flags |= VM_NX;
    return flags;
}

static npk_vma_t *find_vma(npk_vma_t *list, vaddr_t address);
static bool split_vma_at(struct process *process, vaddr_t point);

static npk_vma_t *find_heap_vma(struct process *process) {
    if (!process) return NULL;
    for (npk_vma_t *vma = (npk_vma_t *)process->vmas; vma; vma = vma->next)
        if ((vma->flags & NPK_MAP_HEAP) != 0) return vma;
    return NULL;
}

int vm_map_anonymous(struct process *process, vaddr_t hint, uint64_t length,
                     uint64_t protection, uint64_t flags, vaddr_t *result) {
    if (!process || !process->alive || !result || length == 0 || !(flags & NPK_MAP_ANONYMOUS) ||
        (flags & (NPK_MAP_SHARED | NPK_MAP_PRIVATE)) == 0 || (protection & ~7ULL) ||
        ((protection & NPK_PROT_WRITE) && (protection & NPK_PROT_EXEC))) return VM_EINVAL;
    uint64_t size;
    if (!page_align_up(length, &size) || size == 0 || size > VM_USER_MMAP_LIMIT - VM_USER_MMAP_BASE) return VM_EINVAL;
    vaddr_t start;
    if (flags & NPK_MAP_FIXED) {
        if ((hint & (NPK_PAGE_SIZE - 1)) != 0) return VM_EINVAL;
        start = hint;
    } else {
        start = hint ? page_align(hint) : (process->mmap_cursor ? process->mmap_cursor : VM_USER_MMAP_BASE);
        if (start < VM_USER_MMAP_BASE) start = VM_USER_MMAP_BASE;
        while (add_overflow(start, size) || start + size > VM_USER_MMAP_LIMIT || overlaps((npk_vma_t *)process->vmas, start, start + size)) {
            start += NPK_PAGE_SIZE;
            if (start < VM_USER_MMAP_BASE || start >= VM_USER_MMAP_LIMIT) return VM_ENOMEM;
        }
    }
    if (start < NPK_USER_MIN || add_overflow(start, size) || start + size > NPK_USER_MAX || overlaps((npk_vma_t *)process->vmas, start, start + size)) return VM_EEXIST;

    npk_vma_t *vma = (npk_vma_t *)kmalloc(sizeof(*vma));
    if (!vma) return VM_ENOMEM;
    vma->start = start;
    vma->end = start + size;
    vma->protection = protection;
    vma->flags = flags;
    vma->backing_handle = -1;
    vma->backing_offset = 0;
    vma->backing_size = 0;
    vma->next = (npk_vma_t *)process->vmas;

    /* Anonymous mappings are reservations. Leaf pages are materialized by
     * vm_handle_page_fault() on first user access. This keeps mmap cheap and
     * gives fork/COW a precise set of actually touched pages. */
    process->vmas = vma;
    process->mmap_cursor = vma->end + NPK_PAGE_SIZE;
    *result = start;
    return 0;
}

int vm_map_file(struct process *process, int handle, vaddr_t hint, uint64_t length,
                uint64_t protection, uint64_t flags, uint64_t offset, vaddr_t *result) {
    if (!process || !process->alive || handle < 0 || !result || length == 0 ||
        (flags & NPK_MAP_ANONYMOUS) || (flags & (NPK_MAP_SHARED | NPK_MAP_PRIVATE)) == 0 ||
        (protection & ~7ULL) || ((protection & NPK_PROT_WRITE) && (protection & NPK_PROT_EXEC)) ||
        (offset & (NPK_PAGE_SIZE - 1)) != 0 ||
        ((protection & NPK_PROT_WRITE) && (flags & NPK_MAP_SHARED)) || !vfs_file_backed(handle))
        return VM_EINVAL;
    ssize_t file_size_result = vfs_size(handle);
    if (file_size_result < 0 || offset > UINT64_MAX - (uint64_t)length) return VM_EINVAL;
    uint64_t file_size = (uint64_t)file_size_result;

    /* Reuse the anonymous reservation path for identical address selection and
     * overlap checks, then convert the resulting VMA into a lazy file mapping. */
    vaddr_t mapped = 0;
    int result_code = vm_map_anonymous(process, hint, length, protection,
                                       flags | NPK_MAP_ANONYMOUS, &mapped);
    if (result_code != 0) return result_code;
    if (vfs_retain(handle) != 0) {
        (void)vm_unmap(process, mapped, length);
        return VM_EINVAL;
    }
    npk_vma_t *vma = find_vma((npk_vma_t *)process->vmas, mapped);
    if (!vma) {
        (void)vfs_close(handle);
        (void)vm_unmap(process, mapped, length);
        return VM_EINVAL;
    }
    vma->flags &= ~NPK_MAP_ANONYMOUS;
    vma->backing_handle = handle;
    vma->backing_offset = offset;
    uint64_t available = offset < file_size ? file_size - offset : 0;
    uint64_t mapping_size = vma->end - vma->start;
    vma->backing_size = available < mapping_size ? available : mapping_size;
    *result = mapped;
    return 0;
}

int vm_map_shared(struct process *process, int handle, vaddr_t hint, uint64_t length,
                  uint64_t protection, uint64_t flags, uint64_t offset, vaddr_t *result) {
    if (!process || !process->alive || handle < 0 || !result || length == 0 ||
        (flags & NPK_MAP_ANONYMOUS) || (flags & NPK_MAP_SHARED) == 0 ||
        (flags & NPK_MAP_PRIVATE) != 0 || (protection & ~7ULL) ||
        ((protection & NPK_PROT_WRITE) && (protection & NPK_PROT_EXEC)) ||
        (offset & (NPK_PAGE_SIZE - 1)) != 0 || !vfs_is_shared_memory(handle))
        return VM_EINVAL;
    size_t page_count = vfs_shm_page_count(handle);
    if (page_count == 0 || offset > UINT64_MAX - length ||
        offset + length > (uint64_t)page_count * NPK_PAGE_SIZE)
        return VM_EINVAL;
    vaddr_t mapped = 0;
    int result_code = vm_map_anonymous(process, hint, length, protection,
                                       flags | NPK_MAP_ANONYMOUS, &mapped);
    if (result_code != 0) return result_code;
    if (vfs_retain(handle) != 0) {
        (void)vm_unmap(process, mapped, length);
        return VM_EINVAL;
    }
    npk_vma_t *vma = find_vma((npk_vma_t *)process->vmas, mapped);
    if (!vma) {
        (void)vfs_close(handle);
        (void)vm_unmap(process, mapped, length);
        return VM_EINVAL;
    }
    vma->flags &= ~NPK_MAP_ANONYMOUS;
    vma->flags |= NPK_MAP_SHARED_OBJECT;
    vma->backing_handle = handle;
    vma->backing_offset = offset;
    vma->backing_size = vma->end - vma->start;
    *result = mapped;
    return 0;
}

int vm_map_device(struct process *process, vaddr_t hint, uint64_t physical,
                  uint64_t length, uint64_t protection, uint64_t flags, vaddr_t *result) {
    if (!process || !process->alive || !result || length == 0 ||
        (flags & NPK_MAP_ANONYMOUS) || (flags & NPK_MAP_SHARED) == 0 ||
        (flags & NPK_MAP_PRIVATE) != 0 || (protection & ~7ULL) ||
        (protection & NPK_PROT_EXEC) ||
        (physical & (NPK_PAGE_SIZE - 1)) != 0)
        return VM_EINVAL;
    uint64_t size;
    if (!page_align_up(length, &size) || size == 0 || physical > UINT64_MAX - size)
        return VM_EINVAL;
    vaddr_t mapped = 0;
    int result_code = vm_map_anonymous(process, hint, size, protection,
                                       flags | NPK_MAP_ANONYMOUS, &mapped);
    if (result_code != 0) return result_code;
    npk_vma_t *vma = find_vma((npk_vma_t *)process->vmas, mapped);
    if (!vma) {
        (void)vm_unmap(process, mapped, size);
        return VM_EINVAL;
    }
    vma->flags &= ~NPK_MAP_ANONYMOUS;
    vma->flags |= NPK_MAP_DEVICE;
    for (uint64_t offset = 0; offset < size; offset += NPK_PAGE_SIZE) {
        uint64_t map_flags = page_flags_for(protection) | VM_DEVICE;
        if (!vmm_map_page_root(process->address_space_root, mapped + offset,
                               physical + offset, map_flags)) {
            (void)vm_unmap(process, mapped, size);
            return VM_ENOMEM;
        }
    }
    *result = mapped;
    return 0;
}

int vm_reserve_range(struct process *process, vaddr_t start, uint64_t length,
                      uint64_t protection, uint64_t flags) {
    uint64_t size;
    if (!process || !process->alive || start < NPK_USER_MIN ||
        (start & (NPK_PAGE_SIZE - 1)) != 0 || length == 0 ||
        !page_align_up(length, &size) || size == 0 || add_overflow(start, size) ||
        start + size > NPK_USER_MAX || (protection & ~7ULL) != 0 ||
        ((protection & NPK_PROT_WRITE) && (protection & NPK_PROT_EXEC)) ||
        overlaps((npk_vma_t *)process->vmas, start, start + size)) return VM_EINVAL;
    npk_vma_t *vma = (npk_vma_t *)kmalloc(sizeof(*vma));
    if (!vma) return VM_ENOMEM;
    *vma = (npk_vma_t){
        .start = start, .end = start + size, .protection = protection,
        .flags = flags, .backing_handle = -1, .backing_offset = 0,
        .backing_size = 0, .next = (npk_vma_t *)process->vmas,
    };
    process->vmas = vma;
    return 0;
}

int vm_reserve_heap(struct process *process, vaddr_t start, vaddr_t limit) {
    if (!process || !process->alive || start >= limit ||
        find_heap_vma(process) != NULL) return VM_EINVAL;
    int result = vm_reserve_range(process, start, limit - start,
                                  NPK_PROT_READ | NPK_PROT_WRITE,
                                  NPK_MAP_PRIVATE | NPK_MAP_ANONYMOUS | NPK_MAP_HEAP);
    if (result != 0) return result;
    process->brk_start = start;
    process->brk_end = start;
    return 0;
}

int vm_resize_heap(struct process *process, vaddr_t new_end) {
    npk_vma_t *heap = find_heap_vma(process);
    if (!process || !process->alive || !heap ||
        (new_end & (NPK_PAGE_SIZE - 1)) != 0 ||
        new_end < heap->start || new_end > heap->end) return VM_EINVAL;
    /* The heap VMA is the reservation. Pages are materialized lazily by the
     * regular page-fault path, so growing/shrinking cannot unmap an unrelated
     * mapping and requires no destructive rollback. */
    if (new_end < process->brk_end) {
        for (vaddr_t page = new_end; page < process->brk_end; page += NPK_PAGE_SIZE) {
            paddr_t physical = vmm_unmap_page_root(process->address_space_root, page);
            if (physical) pmm_free_page(physical);
        }
    }
    process->brk_end = new_end;
    return 0;
}

int vm_protect(struct process *process, vaddr_t address, uint64_t length, uint64_t protection) {
    if (!process || !process->alive || (address & (NPK_PAGE_SIZE - 1)) != 0 || length == 0 ||
        (protection & ~7ULL) != 0 ||
        ((protection & NPK_PROT_WRITE) && (protection & NPK_PROT_EXEC)))
        return VM_EINVAL;
    uint64_t size;
    if (!page_align_up(length, &size) || add_overflow(address, size) ||
        address < NPK_USER_MIN || address + size > NPK_USER_MAX)
        return VM_EINVAL;
    vaddr_t end = address + size;
    vaddr_t cursor = address;
    while (cursor < end) {
        npk_vma_t *vma = find_vma((npk_vma_t *)process->vmas, cursor);
        if (!vma || vma->end <= cursor) return VM_EINVAL;
        /* Keep the heap reservation contiguous; its protection is managed by
         * brk/page-fault policy rather than by splitting the brk VMA. */
        if ((vma->flags & NPK_MAP_HEAP) != 0) return VM_EINVAL;
        cursor = vma->end < end ? vma->end : end;
    }
    if (!split_vma_at(process, address) || !split_vma_at(process, end)) return VM_ENOMEM;

    for (npk_vma_t *vma = (npk_vma_t *)process->vmas; vma; vma = vma->next) {
        if (vma->start < address || vma->end > end) continue;
        vma->protection = protection;
    }
    uint64_t flags = page_flags_for(protection);
    for (vaddr_t page = address; page < end; page += NPK_PAGE_SIZE) {
        if (vmm_lookup_root(process->address_space_root, page, VM_USER) != 0 &&
            !vmm_protect_page_root(process->address_space_root, page, flags))
            return VM_EINVAL;
    }
    return 0;
}

int vm_unmap(struct process *process, vaddr_t address, uint64_t length) {
    if (!process || !process->alive || (address & (NPK_PAGE_SIZE - 1)) != 0 || length == 0) return VM_EINVAL;
    uint64_t size;
    if (!page_align_up(length, &size) || add_overflow(address, size)) return VM_EINVAL;
    vaddr_t end = address + size;
    npk_vma_t *previous = NULL;
    npk_vma_t *vma = (npk_vma_t *)process->vmas;
    while (vma && !(address >= vma->start && end <= vma->end)) { previous = vma; vma = vma->next; }
    if (!vma) return VM_EINVAL;

    if ((vma->flags & NPK_MAP_HEAP) != 0) return VM_EINVAL;
    npk_vma_t *right = NULL;
    if (address > vma->start && end < vma->end) {
        right = (npk_vma_t *)kmalloc(sizeof(*right));
        if (!right) return VM_ENOMEM;
        *right = *vma;
        right->start = end;
        right->next = vma->next;
        if (vma->backing_handle >= 0) {
            if (vfs_retain(vma->backing_handle) != 0) {
                kfree(right);
                return VM_ENOMEM;
            }
            uint64_t right_delta = end - vma->start;
            right->backing_offset += right_delta;
            right->backing_size = right_delta < vma->backing_size ? vma->backing_size - right_delta : 0;
            uint64_t left_size = address - vma->start;
            vma->backing_size = left_size < vma->backing_size ? left_size : vma->backing_size;
        }
        vma->end = address;
        vma->next = right;
    } else if (address == vma->start && end == vma->end) {
        if (previous) previous->next = vma->next; else process->vmas = vma->next;
    } else if (address == vma->start) {
        uint64_t delta = end - vma->start;
        vma->start = end;
        if (vma->backing_handle >= 0) {
            vma->backing_offset += delta;
            vma->backing_size = delta < vma->backing_size ? vma->backing_size - delta : 0;
        }
    } else if (end == vma->end) {
        uint64_t left_size = address - vma->start;
        vma->end = address;
        if (vma->backing_handle >= 0)
            vma->backing_size = left_size < vma->backing_size ? left_size : vma->backing_size;
    } else return VM_EINVAL;

    for (vaddr_t current = address; current < end; current += NPK_PAGE_SIZE) {
        paddr_t physical = vmm_unmap_page_root(process->address_space_root, current);
        if (physical) pmm_free_page(physical);
    }
    if (address == vma->start && end == vma->end) {
        if (vma->backing_handle >= 0) (void)vfs_close(vma->backing_handle);
        kfree(vma);
    }
    return 0;
}

void vm_revoke_device_mappings(struct process *process) {
    if (!process) return;
    npk_vma_t *vma = (npk_vma_t *)process->vmas;
    while (vma) {
        npk_vma_t *next = vma->next;
        if ((vma->flags & NPK_MAP_DEVICE) != 0)
            (void)vm_unmap(process, vma->start, vma->end - vma->start);
        vma = next;
    }
}

bool vm_range_owned(struct process *process, vaddr_t address, uint64_t length, uint64_t protection) {
    if (!process || !vmm_is_user_range(address, length) || add_overflow(address, length)) return false;
    vaddr_t end = address + length;
    for (npk_vma_t *vma = (npk_vma_t *)process->vmas; vma; vma = vma->next)
        if (address >= vma->start && end <= vma->end && (vma->protection & protection) == protection) return true;
    return false;
}

void vm_destroy_all(struct process *process) {
    if (!process) return;
    npk_vma_t *vma = (npk_vma_t *)process->vmas;
    while (vma) {
        npk_vma_t *next = vma->next;
        if (vma->backing_handle >= 0) (void)vfs_close(vma->backing_handle);
        kfree(vma);
        vma = next;
    }
    process->vmas = NULL;
}

bool vm_clone_metadata(const struct process *parent, struct process *child) {
    if (!parent || !child) return false;
    child->vmas = NULL;
    npk_vma_t **tail = (npk_vma_t **)&child->vmas;
    for (const npk_vma_t *source = (const npk_vma_t *)parent->vmas; source; source = source->next) {
        if ((source->flags & NPK_MAP_DEVICE) != 0) continue;
        npk_vma_t *copy = (npk_vma_t *)kmalloc(sizeof(*copy));
        if (!copy) {
            vm_destroy_all(child);
            return false;
        }
        *copy = *source;
        copy->next = NULL;
        if (copy->backing_handle >= 0 && vfs_retain(copy->backing_handle) != 0) {
            kfree(copy);
            vm_destroy_all(child);
            return false;
        }
        *tail = copy;
        tail = &copy->next;
    }
    return true;
}

static npk_vma_t *find_vma(npk_vma_t *list, vaddr_t address) {
    for (npk_vma_t *vma = list; vma; vma = vma->next)
        if (address >= vma->start && address < vma->end) return vma;
    return NULL;
}

static bool split_vma_at(struct process *process, vaddr_t point) {
    if (!process) return false;
    for (npk_vma_t *vma = (npk_vma_t *)process->vmas; vma; vma = vma->next) {
        if (point <= vma->start || point >= vma->end) continue;
        npk_vma_t *right = (npk_vma_t *)kmalloc(sizeof(*right));
        if (!right) return false;
        *right = *vma;
        right->start = point;
        right->next = vma->next;
        if (vma->backing_handle >= 0) {
            if (vfs_retain(vma->backing_handle) != 0) {
                kfree(right);
                return false;
            }
            uint64_t delta = point - vma->start;
            right->backing_offset += delta;
            right->backing_size = delta < vma->backing_size ? vma->backing_size - delta : 0;
            vma->backing_size = delta < vma->backing_size ? delta : vma->backing_size;
        }
        vma->end = point;
        vma->next = right;
        return true;
    }
    return true;
}

static bool map_zero_page(process_t *process, vaddr_t page, uint64_t protection) {
    if (!process || !process->address_space_root || !vmm_is_user_range(page, NPK_PAGE_SIZE)) return false;
    uint64_t flags = page_flags_for(protection);
    if ((protection & NPK_PROT_WRITE) == 0) flags &= ~VM_WRITE;
    paddr_t physical = pmm_alloc_page();
    if (!physical) return false;
    if (!vmm_map_page_root(process->address_space_root, page, physical, flags)) {
        pmm_free_page(physical);
        return false;
    }
    memset(phys_to_virt(physical), 0, NPK_PAGE_SIZE);
    return true;
}

static bool map_file_page(process_t *process, npk_vma_t *vma, vaddr_t page) {
    if (!process || !vma || vma->backing_handle < 0 || page < vma->start || page >= vma->end ||
        !vmm_is_user_range(page, NPK_PAGE_SIZE)) return false;
    paddr_t physical = pmm_alloc_page();
    if (!physical) return false;
    uint8_t *destination = (uint8_t *)phys_to_virt(physical);
    memset(destination, 0, NPK_PAGE_SIZE);
    uint64_t delta = page - vma->start;
    if (delta < vma->backing_size) {
        uint64_t amount = vma->backing_size - delta;
        if (amount > NPK_PAGE_SIZE) amount = NPK_PAGE_SIZE;
        uint64_t file_offset = vma->backing_offset + delta;
        ssize_t copied = vfs_read_at(vma->backing_handle, destination, (size_t)amount, file_offset);
        if (copied != (ssize_t)amount) {
            pmm_free_page(physical);
            return false;
        }
    }
    uint64_t flags = page_flags_for(vma->protection);
    if (!vmm_map_page_root(process->address_space_root, page, physical, flags)) {
        pmm_free_page(physical);
        return false;
    }
    return true;
}

static bool map_shared_page(process_t *process, npk_vma_t *vma, vaddr_t page) {
    if (!process || !vma || vma->backing_handle < 0 || page < vma->start || page >= vma->end ||
        !vmm_is_user_range(page, NPK_PAGE_SIZE)) return false;
    uint64_t delta = page - vma->start;
    if (delta >= vma->backing_size) return false;
    size_t page_index = (size_t)((vma->backing_offset + delta) / NPK_PAGE_SIZE);
    paddr_t physical = vfs_shm_page(vma->backing_handle, page_index);
    if (!physical) return false;
    pmm_retain_page(physical);
    uint64_t flags = page_flags_for(vma->protection) | VM_SHARED;
    if (!vmm_map_page_root(process->address_space_root, page, physical, flags)) {
        pmm_free_page(physical);
        return false;
    }
    return true;
}

static bool stack_page_allowed(process_t *process, vaddr_t page, vaddr_t user_rsp,
                               bool from_kernel) {
    if (!process || process->user_stack_top == 0 || process->user_stack_limit == 0) return false;
    if (page < process->user_stack_limit || page >= process->user_stack_top) return false;
    if (page >= process->user_stack_bottom) return true;
    if (from_kernel) return true;
    if (user_rsp < process->user_stack_limit || user_rsp > process->user_stack_top) return false;
    vaddr_t rsp_page = user_rsp & ~(NPK_PAGE_SIZE - 1);
    return page + NPK_PAGE_SIZE >= rsp_page;
}

bool vm_ensure_user_page(process_t *process, vaddr_t address, bool write) {
    if (!process || !process->alive || !vmm_is_user_range(address, 1)) return false;
    vaddr_t page = address & ~(NPK_PAGE_SIZE - 1);
    uint64_t required = VM_USER | (write ? VM_WRITE : 0);
    if (vmm_lookup_root(process->address_space_root, address, required) != 0) return true;
    npk_vma_t *vma = find_vma((npk_vma_t *)process->vmas, address);
    uint64_t protection = vma ? vma->protection : 0;
    bool stack = stack_page_allowed(process, page, 0, true);
    if (vma && (vma->flags & NPK_MAP_HEAP) != 0 && address >= process->brk_end) return false;
    if (!vma && !stack) return false;
    if (write && !(protection & NPK_PROT_WRITE) && !stack) return false;
    if (!write && !(protection & (NPK_PROT_READ | NPK_PROT_WRITE | NPK_PROT_EXEC)) && !stack) return false;
    if (stack) protection = NPK_PROT_READ | NPK_PROT_WRITE;
    bool mapped = vma && (vma->flags & NPK_MAP_SHARED_OBJECT) != 0 ? map_shared_page(process, vma, page) :
                  (vma && vma->backing_handle >= 0 ? map_file_page(process, vma, page) :
                   map_zero_page(process, page, protection));
    if (!mapped) return false;
    if (stack && page < process->user_stack_bottom) process->user_stack_bottom = page;
    return true;
}

bool vm_handle_page_fault(process_t *process, vaddr_t fault_address,
                          uint64_t error_code, vaddr_t user_rsp) {
    if (!process || !process->alive || !vmm_is_user_range(fault_address, 1)) return false;
    bool write = (error_code & 2ULL) != 0;
    if (error_code & 1ULL) {
        npk_vma_t *vma = find_vma((npk_vma_t *)process->vmas, fault_address);
        if (!write || !vma || !(vma->protection & NPK_PROT_WRITE)) return false;
        return vmm_resolve_cow_fault(process->address_space_root, fault_address);
    }
    bool instruction = (error_code & 16ULL) != 0;
    if (instruction || !vmm_is_user_range(fault_address & ~(NPK_PAGE_SIZE - 1), NPK_PAGE_SIZE)) return false;
    vaddr_t page = fault_address & ~(NPK_PAGE_SIZE - 1);
    npk_vma_t *vma = find_vma((npk_vma_t *)process->vmas, fault_address);
    bool stack = stack_page_allowed(process, page, user_rsp, false);
    if (vma && (vma->flags & NPK_MAP_HEAP) != 0 && fault_address >= process->brk_end) return false;
    if (!vma && !stack) return false;
    uint64_t protection = stack ? (NPK_PROT_READ | NPK_PROT_WRITE) : vma->protection;
    if (write && !(protection & NPK_PROT_WRITE)) return false;
    if (!write && !(protection & (NPK_PROT_READ | NPK_PROT_WRITE | NPK_PROT_EXEC))) return false;
    bool mapped = vma && (vma->flags & NPK_MAP_SHARED_OBJECT) != 0 ? map_shared_page(process, vma, page) :
                  (vma && vma->backing_handle >= 0 ? map_file_page(process, vma, page) :
                   map_zero_page(process, page, protection));
    if (!mapped) return false;
    if (stack && page < process->user_stack_bottom) process->user_stack_bottom = page;
    return true;
}
