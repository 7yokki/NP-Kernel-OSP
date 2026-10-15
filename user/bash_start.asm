BITS 64
SECTION .text
GLOBAL _start
EXTERN bash_main
_start:
    xor ebp, ebp
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    and rsp, -16
    call bash_main
    mov edi, eax
    mov eax, 60
    syscall
.hang:
    hlt
    jmp .hang
