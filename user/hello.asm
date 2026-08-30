BITS 64
SECTION .text
GLOBAL _start
_start:
    mov eax, 60
    xor edi, edi
    syscall
    hlt
