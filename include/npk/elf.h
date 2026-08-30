#ifndef NPK_ELF_H
#define NPK_ELF_H

#include "types.h"

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 62
#define EV_CURRENT 1
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_PHDR 6
#define PT_GNU_STACK 0x6474e551U
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define NPK_USER_MIN 0x0000000000010000ULL
#define NPK_USER_MAX 0x0000800000000000ULL
#define NPK_USER_DYN_BASE 0x0000004000000000ULL
/* Keep the interpreter above the main ET_DYN window; both bases are page-aligned. */
#define NPK_USER_INTERP_BASE 0x0000005000000000ULL
#define NPK_ELF_INTERP_MAX 256U

typedef struct NPK_PACKED {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf64_header_t;

typedef struct NPK_PACKED {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf64_program_header_t;

typedef struct {
    uint64_t entry;
    uint64_t image_base;
    uint64_t image_end;
    uint64_t load_bias;
    uint64_t stack_top;
    paddr_t address_space_root;
    uint16_t loaded_segments;
    uint64_t program_header;
    uint16_t program_header_count;
    uint16_t program_header_size;
    uint8_t has_interp;
    char interpreter[NPK_ELF_INTERP_MAX];
} elf_load_result_t;

bool elf64_validate(const void *image, size_t image_size);
int elf64_load(const void *image, size_t image_size, elf_load_result_t *result);
int elf64_load_in_address_space(const void *image, size_t image_size, paddr_t root,
                                uint64_t load_bias, elf_load_result_t *result);
int elf64_get_interpreter(const void *image, size_t image_size,
                          char *path, size_t path_capacity);

#endif
