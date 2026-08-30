#include <npk/elf.h>
#include <npk/heap.h>
#include <npk/log.h>
#include <npk/memory.h>
#include <npk/string.h>

#define NPK_MAX_PHNUM 64U
#define NPK_MAX_ELF_PAGES 65536U
#define ELF_EINVAL -22
#define ELF_EFAULT -14
#define ELF_ENOMEM -12
#define ELF_ENOEXEC -8
#define ELF_EPERM -1
#define ELF_ENOENT -2
#define ELF_DYNAMIC_ENTRY_SIZE 16U
#define ELF_DT_NULL 0

typedef struct {
    vaddr_t virtual_address;
    paddr_t physical_address;
} elf_page_record_t;

static bool add_overflows(uint64_t a, uint64_t b) { return b > UINT64_MAX - a; }
static bool mul_overflows(uint64_t a, uint64_t b) { return a != 0 && b > UINT64_MAX / a; }
static bool is_power_of_two(uint64_t value) { return value && !(value & (value - 1)); }
static uint64_t align_down(uint64_t value) { return value & ~(NPK_PAGE_SIZE - 1); }
static bool align_up(uint64_t value, uint64_t *result) {
    if (value > UINT64_MAX - (NPK_PAGE_SIZE - 1)) return false;
    *result = (value + NPK_PAGE_SIZE - 1) & ~(NPK_PAGE_SIZE - 1);
    return true;
}

static bool user_span(uint64_t start, uint64_t length, uint64_t bias, uint64_t *absolute_end) {
    if (add_overflows(start, bias)) return false;
    uint64_t absolute = start + bias;
    if (absolute < NPK_USER_MIN || absolute >= NPK_USER_MAX || length == 0 || add_overflows(absolute, length)) return false;
    uint64_t end = absolute + length;
    if (end > NPK_USER_MAX || end <= absolute) return false;
    *absolute_end = end;
    return true;
}

static bool segment_range_valid(const elf64_program_header_t *ph, size_t image_size,
                                uint64_t bias, uint64_t *start, uint64_t *end,
                                uint64_t *page_start, uint64_t *page_end) {
    if (ph->filesz > ph->memsz || ph->memsz == 0) return false;
    if (ph->offset > image_size || ph->filesz > image_size - ph->offset) return false;
    if (ph->align > 1) {
        if (!is_power_of_two(ph->align) || (ph->vaddr % ph->align) != (ph->offset % ph->align)) return false;
    }
    if ((ph->flags & PF_W) && (ph->flags & PF_X)) return false;
    if (!user_span(ph->vaddr, ph->memsz, bias, end)) return false;
    *start = ph->vaddr + bias;
    if (*start < NPK_USER_MIN) return false;
    *page_start = align_down(*start);
    return align_up(*end, page_end) && *page_end <= NPK_USER_MAX && *page_end > *page_start;
}

static bool load_segments_overlap(const elf64_program_header_t *programs, uint16_t index, size_t image_size,
                                  uint64_t bias, uint64_t current_start, uint64_t current_end) {
    for (uint16_t j = 0; j < index; ++j) {
        if (programs[j].type != PT_LOAD) continue;
        uint64_t start, end, page_start, page_end;
        if (!segment_range_valid(&programs[j], image_size, bias, &start, &end, &page_start, &page_end)) return true;
        if (current_start < page_end && start < current_end) return true;
    }
    return false;
}

typedef struct NPK_PACKED {
    int64_t tag;
    uint64_t value;
} elf64_dynamic_t;

static bool dynamic_table_valid(const uint8_t *bytes, size_t image_size,
                                const elf64_program_header_t *ph) {
    if (!bytes || !ph || ph->filesz == 0 || ph->filesz > ph->memsz ||
        (ph->filesz % ELF_DYNAMIC_ENTRY_SIZE) != 0 ||
        ph->offset > image_size || ph->filesz > image_size - ph->offset) return false;
    const elf64_dynamic_t *entries = (const elf64_dynamic_t *)(bytes + ph->offset);
    size_t count = (size_t)(ph->filesz / ELF_DYNAMIC_ENTRY_SIZE);
    bool terminated = false;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].tag == ELF_DT_NULL) {
            terminated = true;
            break;
        }
        /* NPKernel maps the dynamic interpreter and leaves relocation policy to
         * user-space ld.so. Reject tags whose payload would be an unbounded
         * in-image pointer instead of trusting arbitrary metadata. */
        if (entries[i].tag < 0) return false;
    }
    return terminated;
}

static bool interpreter_field_valid(const uint8_t *bytes, size_t image_size,
                                    const elf64_program_header_t *ph) {
    if (!bytes || !ph || ph->filesz == 0 || ph->filesz > ph->memsz ||
        ph->filesz > NPK_ELF_INTERP_MAX || ph->offset > image_size ||
        ph->filesz > image_size - ph->offset) return false;
    const uint8_t *value = bytes + ph->offset;
    if (value[ph->filesz - 1] != 0 || value[0] != '/') return false;
    size_t component_start = 1;
    for (uint64_t i = 0; i + 1 < ph->filesz; ++i) {
        if (value[i] == 0 || value[i] < 0x20 || value[i] == 0x7f) return false;
        if (value[i] != '/' && i + 1 != ph->filesz - 1) continue;
        size_t component_length = (size_t)i - component_start;
        if (component_length == 2 && value[component_start] == '.' &&
            value[component_start + 1] == '.') return false;
        component_start = (size_t)i + 1;
    }
    return true;
}

static bool header_valid(const void *image, size_t image_size, const elf64_header_t **out) {
    if (image == NULL || out == NULL || image_size < sizeof(elf64_header_t)) return false;
    const elf64_header_t *header = (const elf64_header_t *)image;
    if (header->ident[0] != 0x7f || header->ident[1] != 'E' || header->ident[2] != 'L' || header->ident[3] != 'F') return false;
    if (header->ident[4] != ELFCLASS64 || header->ident[5] != ELFDATA2LSB || header->ident[6] != EV_CURRENT) return false;
    if (header->type != ET_EXEC && header->type != ET_DYN) return false;
    if (header->machine != EM_X86_64 || header->version != EV_CURRENT) return false;
    if (header->ehsize != sizeof(elf64_header_t) || header->phentsize != sizeof(elf64_program_header_t) ||
        header->phnum == 0 || header->phnum > NPK_MAX_PHNUM) return false;
    if (mul_overflows(header->phnum, header->phentsize) || add_overflows(header->phoff, (uint64_t)header->phnum * header->phentsize) ||
        header->phoff > image_size || (uint64_t)header->phnum * header->phentsize > image_size - header->phoff) return false;
    *out = header;
    return true;
}

static bool elf64_validate_at_bias(const void *image, size_t image_size, uint64_t bias) {
    const elf64_header_t *header;
    if (!header_valid(image, image_size, &header)) return false;
    if ((bias & (NPK_PAGE_SIZE - 1)) != 0) return false;
    if (header->type == ET_EXEC && bias != 0) return false;
    if (header->type == ET_DYN && bias < NPK_USER_DYN_BASE) return false;
    const uint8_t *bytes = (const uint8_t *)image;
    const elf64_program_header_t *programs = (const elf64_program_header_t *)(bytes + header->phoff);
    uint64_t page_count = 0;
    uint64_t absolute_entry;
    if (add_overflows(header->entry, bias)) return false;
    absolute_entry = header->entry + bias;
    uint16_t loaded = 0;
    bool entry_valid = false;
    bool seen_interp = false;
    bool seen_dynamic = false;

    for (uint16_t i = 0; i < header->phnum; ++i) {
        const elf64_program_header_t *ph = &programs[i];
        if (ph->type == PT_INTERP) {
            if (seen_interp || !interpreter_field_valid(bytes, image_size, ph)) return false;
            seen_interp = true;
            continue;
        }
        if (ph->type == PT_DYNAMIC) {
            if (seen_dynamic || !dynamic_table_valid(bytes, image_size, ph)) return false;
            seen_dynamic = true;
            continue;
        }
        if (ph->type != PT_LOAD) continue;
        uint64_t start, end, page_start, page_end;
        if (!segment_range_valid(ph, image_size, bias, &start, &end, &page_start, &page_end)) return false;
        if (load_segments_overlap(programs, i, image_size, bias, page_start, page_end)) return false;
        if (page_end - page_start > NPK_MAX_ELF_PAGES * NPK_PAGE_SIZE - page_count * NPK_PAGE_SIZE) return false;
        page_count += (page_end - page_start) / NPK_PAGE_SIZE;
        if (absolute_entry >= start && absolute_entry < end && (ph->flags & PF_X)) entry_valid = true;
        ++loaded;
    }
    if (loaded == 0 || page_count == 0 || page_count > NPK_MAX_ELF_PAGES || !entry_valid) return false;
    return true;
}

bool elf64_validate(const void *image, size_t image_size) {
    const elf64_header_t *header;
    if (!header_valid(image, image_size, &header)) return false;
    uint64_t bias = header->type == ET_DYN ? NPK_USER_DYN_BASE : 0;
    return elf64_validate_at_bias(image, image_size, bias);
}

int elf64_get_interpreter(const void *image, size_t image_size,
                          char *path, size_t path_capacity) {
    const elf64_header_t *header;
    if (!path || path_capacity < 2 || !header_valid(image, image_size, &header)) return ELF_ENOEXEC;
    const uint8_t *bytes = (const uint8_t *)image;
    const elf64_program_header_t *programs = (const elf64_program_header_t *)(bytes + header->phoff);
    for (uint16_t i = 0; i < header->phnum; ++i) {
        const elf64_program_header_t *ph = &programs[i];
        if (ph->type != PT_INTERP) continue;
        if (!interpreter_field_valid(bytes, image_size, ph) || ph->filesz >= path_capacity) return ELF_ENOEXEC;
        memcpy(path, bytes + ph->offset, (size_t)ph->filesz);
        return (int)ph->filesz - 1;
    }
    path[0] = 0;
    return 0;
}

static uint64_t locate_program_header(const elf64_header_t *header,
                                      const elf64_program_header_t *programs,
                                      uint64_t bias) {
    if (!header || !programs) return 0;
    uint64_t table_size = (uint64_t)header->phnum * header->phentsize;
    for (uint16_t i = 0; i < header->phnum; ++i)
        if (programs[i].type == PT_PHDR && programs[i].vaddr <= UINT64_MAX - bias)
            return programs[i].vaddr + bias;
    for (uint16_t i = 0; i < header->phnum; ++i) {
        const elf64_program_header_t *ph = &programs[i];
        if (ph->type != PT_LOAD || header->phoff < ph->offset ||
            table_size > ph->filesz || header->phoff - ph->offset > ph->filesz - table_size ||
            ph->vaddr > UINT64_MAX - (header->phoff - ph->offset) ||
            ph->vaddr + (header->phoff - ph->offset) > UINT64_MAX - bias) continue;
        return ph->vaddr + (header->phoff - ph->offset) + bias;
    }
    return 0;
}

int elf64_load_in_address_space(const void *image, size_t image_size, paddr_t root,
                                uint64_t load_bias, elf_load_result_t *result) {
    const elf64_header_t *header;
    if (result == NULL || !root || !header_valid(image, image_size, &header)) return ELF_EINVAL;
    if ((load_bias & (NPK_PAGE_SIZE - 1)) != 0) return ELF_EINVAL;
    if (header->type == ET_EXEC && load_bias != 0) return ELF_EINVAL;
    if (header->type == ET_DYN && load_bias < NPK_USER_DYN_BASE) return ELF_EINVAL;
    if (!elf64_validate_at_bias(image, image_size, load_bias)) return ELF_ENOEXEC;

    const uint8_t *bytes = (const uint8_t *)image;
    const elf64_program_header_t *programs = (const elf64_program_header_t *)(bytes + header->phoff);
    uint64_t low = UINT64_MAX, high = 0, page_count = 0;
    uint16_t loaded = 0;
    bool entry_valid = false;

    for (uint16_t i = 0; i < header->phnum; ++i) {
        const elf64_program_header_t *ph = &programs[i];
        if (ph->type != PT_LOAD) continue;
        uint64_t start, end, page_start, page_end;
        if (!segment_range_valid(ph, image_size, load_bias, &start, &end, &page_start, &page_end)) return ELF_ENOEXEC;
        low = start < low ? start : low;
        high = end > high ? end : high;
        page_count += (page_end - page_start) / NPK_PAGE_SIZE;
        if (header->entry + load_bias >= start && header->entry + load_bias < end && (ph->flags & PF_X)) entry_valid = true;
        ++loaded;
    }
    if (!entry_valid || page_count == 0 || page_count > NPK_MAX_ELF_PAGES) return ELF_ENOEXEC;

    elf_page_record_t *records = (elf_page_record_t *)kcalloc((size_t)page_count, sizeof(*records));
    if (!records) return ELF_ENOMEM;
    uint64_t record_count = 0;
    int error = 0;

    for (uint16_t i = 0; i < header->phnum && error == 0; ++i) {
        const elf64_program_header_t *ph = &programs[i];
        if (ph->type != PT_LOAD) continue;
        uint64_t start, end, page_start, page_end;
        if (!segment_range_valid(ph, image_size, load_bias, &start, &end, &page_start, &page_end)) { error = ELF_ENOEXEC; break; }
        uint64_t flags = VM_USER;
        if (ph->flags & PF_W) flags |= VM_WRITE;
        if (!(ph->flags & PF_X)) flags |= VM_NX;
        for (uint64_t va = page_start; va < page_end; va += NPK_PAGE_SIZE) {
            paddr_t physical = pmm_alloc_page();
            if (physical == 0 || record_count >= page_count || !vmm_map_page_root(root, va, physical, flags)) {
                if (physical) pmm_free_page(physical);
                error = ELF_ENOMEM;
                break;
            }
            uint8_t *destination = (uint8_t *)phys_to_virt(physical);
            memset(destination, 0, NPK_PAGE_SIZE);
            uint64_t file_start = ph->vaddr + load_bias;
            uint64_t file_end = file_start + ph->filesz;
            uint64_t copy_start = start > va ? start : va;
            uint64_t copy_end = file_end < va + NPK_PAGE_SIZE ? file_end : va + NPK_PAGE_SIZE;
            if (copy_end > copy_start) {
                uint64_t source_delta = copy_start - file_start;
                if (source_delta > ph->filesz || copy_end - copy_start > ph->filesz - source_delta) { error = ELF_EFAULT; break; }
                memcpy(destination + (copy_start - va), bytes + ph->offset + source_delta, (size_t)(copy_end - copy_start));
            }
            records[record_count++] = (elf_page_record_t){ .virtual_address = va, .physical_address = physical };
        }
    }

    if (error == 0) {
        result->entry = header->entry + load_bias;
        result->image_base = low;
        result->image_end = high;
        result->load_bias = load_bias;
        result->stack_top = 0;
        result->address_space_root = root;
        result->loaded_segments = loaded;
        result->program_header = locate_program_header(header, programs, load_bias);
        result->program_header_count = header->phnum;
        result->program_header_size = header->phentsize;
        result->has_interp = 0;
        result->interpreter[0] = 0;
        (void)elf64_get_interpreter(image, image_size, result->interpreter,
                                    sizeof(result->interpreter));
        result->has_interp = result->interpreter[0] != 0;
        kfree(records);
        LOG_INFOF("elf", "secure PT_LOAD pages", record_count);
        LOG_INFOF("elf", "entry point", result->entry);
        return 0;
    }

    while (record_count > 0) {
        --record_count;
        paddr_t physical = vmm_unmap_page_root(root, records[record_count].virtual_address);
        if (physical) pmm_free_page(physical);
    }
    kfree(records);
    return error;
}

int elf64_load(const void *image, size_t image_size, elf_load_result_t *result) {
    const elf64_header_t *header;
    if (!header_valid(image, image_size, &header)) return ELF_EINVAL;
    uint64_t bias = header->type == ET_DYN ? NPK_USER_DYN_BASE : 0;
    return elf64_load_in_address_space(image, image_size, vmm_current_root(), bias, result);
}
