OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS {
    /* Limine v9 rejects lower-half PT_LOAD addresses; use the canonical higher half. */
    . = 0xffffffff80000000;

    .text ALIGN(CONSTANT (MAXPAGESIZE)) : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }

    .rodata ALIGN(CONSTANT (MAXPAGESIZE)) : {
        *(.rodata .rodata.*)
    }

    .limine_requests ALIGN(CONSTANT (MAXPAGESIZE)) : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    }

    .data ALIGN(CONSTANT (MAXPAGESIZE)) : {
        *(.got .got.*)
        *(.data .data.*)
    }

    .bss ALIGN(CONSTANT (MAXPAGESIZE)) : {
        *(COMMON)
        *(.bss .bss.*)
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
    }
}
