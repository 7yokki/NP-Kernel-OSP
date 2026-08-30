BITS 64

SECTION .text
GLOBAL _start

_start:
    ; void *mmap(NULL, 4096, PROT_READ|PROT_WRITE,
    ;            MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    mov rax, 9
    xor rdi, rdi
    mov rsi, 4096
    mov rdx, 3
    mov r10, 34
    mov r8, -1
    xor r9, r9
    syscall
    test rax, rax
    js .fail
    mov byte [rax], 0x42
    mov rdi, 0
    mov rax, 60
    syscall
.fail:
    mov rdi, 1
    mov rax, 60
    syscall
