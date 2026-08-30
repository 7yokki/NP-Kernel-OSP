BITS 64

section .data
shared_value: db 0x11

section .text
global _start
_start:
    mov rax, 57                 ; fork
    syscall
    test rax, rax
    jnz .parent

    ; Child writes the inherited writable data page. This must resolve COW.
    mov byte [rel shared_value], 0x42
    xor rdi, rdi
    mov rax, 60                 ; exit(0)
    .parent:
    mov rdi, -1                ; wait for any child
    xor rsi, rsi               ; no status pointer
    xor rdx, rdx               ; blocking wait4
    mov rax, 61                ; wait4
    syscall
    xor rdi, rdi
    mov rax, 60                 ; exit(0)
    syscall

.hang:
    hlt
    jmp .hang
