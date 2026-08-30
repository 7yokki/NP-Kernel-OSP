SECTION .text
GLOBAL _start
GLOBAL isr_stub_default
GLOBAL isr_stub_divide_error
GLOBAL isr_stub_debug
GLOBAL isr_stub_nmi
GLOBAL isr_stub_breakpoint
GLOBAL isr_stub_overflow
GLOBAL isr_stub_bound_range
GLOBAL isr_stub_invalid_opcode
GLOBAL isr_stub_device_not_available
GLOBAL isr_stub_double_fault
GLOBAL isr_stub_coprocessor_overrun
GLOBAL isr_stub_invalid_tss
GLOBAL isr_stub_segment_not_present
GLOBAL isr_stub_stack_segment
GLOBAL isr_stub_general_protection
GLOBAL isr_stub_page_fault
GLOBAL isr_stub_x87
GLOBAL isr_stub_alignment_check
GLOBAL isr_stub_machine_check
GLOBAL isr_stub_simd
GLOBAL isr_stub_virtualization
GLOBAL isr_stub_control_protection
GLOBAL irq1_stub
GLOBAL irq12_stub
GLOBAL irq9_stub
GLOBAL irq_timer_stub
GLOBAL syscall_entry
GLOBAL enter_user_mode
GLOBAL context_switch
GLOBAL restore_interrupt_frame
GLOBAL restore_user_frame_from_kernel
GLOBAL restore_kernel_frame
EXTERN kernel_main
EXTERN exception_dispatch
EXTERN general_protection_dispatch
EXTERN page_fault_dispatch
EXTERN irq1_dispatch
EXTERN irq12_dispatch
EXTERN irq9_dispatch
EXTERN irq_timer_dispatch
EXTERN scheduler_preempt
EXTERN syscall_dispatch_asm

_start:
    cli
    lea rsp, [rel stack_top]
    and rsp, -16
    call kernel_main
.hang:
    hlt
    jmp .hang

; Every exception wrapper preserves the interrupted register state and
; passes a complete wrapper frame to C. The CPU does not perform SWAPGS for
; exceptions, so the saved CS controls both entry and return GS transitions.
%macro SAVE_EXCEPTION_GPRS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro RESTORE_EXCEPTION_GPRS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

%macro EXCEPTION_STUB_NOERROR 2
isr_stub_%1:
    cld
    SAVE_EXCEPTION_GPRS
    mov rax, [rsp + 128]       ; saved CS after fifteen GPRs
    test al, 3
    jz %%kernel_gs
    swapgs
%%kernel_gs:
    mov rdi, rsp
    mov esi, %2
    xor edx, edx               ; no CPU-pushed error code
    call exception_dispatch
    mov rax, [rsp + 128]
    test al, 3
    jz %%restore_gs
    swapgs
%%restore_gs:
    RESTORE_EXCEPTION_GPRS
    iretq
%endmacro

%macro EXCEPTION_STUB_ERROR 2
isr_stub_%1:
    cld
    SAVE_EXCEPTION_GPRS
    mov rax, [rsp + 136]       ; saved CS after GPRs and error code
    test al, 3
    jz %%kernel_gs
    swapgs
%%kernel_gs:
    mov rdi, rsp
    mov esi, %2
    mov edx, 1                ; CPU-pushed error code is present
    call exception_dispatch
    mov rax, [rsp + 136]
    test al, 3
    jz %%restore_gs
    swapgs
%%restore_gs:
    RESTORE_EXCEPTION_GPRS
    add rsp, 8                 ; discard CPU-pushed error code
    iretq
%endmacro

; The default gate is also made safe for an unexpected user-originated
; vector. Its synthetic vector value is outside the architectural exception
; range and therefore follows the conservative signal mapping.
 isr_stub_default:
    cld
    SAVE_EXCEPTION_GPRS
    mov rax, [rsp + 128]
    test al, 3
    jz .default_kernel_gs
    swapgs
.default_kernel_gs:
    mov rdi, rsp
    mov esi, 255
    xor edx, edx
    call exception_dispatch
    mov rax, [rsp + 128]
    test al, 3
    jz .default_restore_gs
    swapgs
.default_restore_gs:
    RESTORE_EXCEPTION_GPRS
    iretq

EXCEPTION_STUB_NOERROR divide_error, 0
EXCEPTION_STUB_NOERROR debug, 1
EXCEPTION_STUB_NOERROR nmi, 2
EXCEPTION_STUB_NOERROR breakpoint, 3
EXCEPTION_STUB_NOERROR overflow, 4
EXCEPTION_STUB_NOERROR bound_range, 5
EXCEPTION_STUB_NOERROR invalid_opcode, 6
EXCEPTION_STUB_NOERROR device_not_available, 7
EXCEPTION_STUB_ERROR double_fault, 8
EXCEPTION_STUB_NOERROR coprocessor_overrun, 9
EXCEPTION_STUB_ERROR invalid_tss, 10
EXCEPTION_STUB_ERROR segment_not_present, 11
EXCEPTION_STUB_ERROR stack_segment, 12

isr_stub_general_protection:
    cld
    SAVE_EXCEPTION_GPRS
    mov rax, [rsp + 136]
    test al, 3
    jz .gp_kernel_gs
    swapgs
.gp_kernel_gs:
    mov rdi, rsp
    call general_protection_dispatch
    mov rax, [rsp + 136]
    test al, 3
    jz .gp_restore_gs
    swapgs
.gp_restore_gs:
    RESTORE_EXCEPTION_GPRS
    add rsp, 8
    iretq

; CPU pushes error, RIP, CS, RFLAGS and (from CPL3) RSP, SS.
isr_stub_page_fault:
    cld
    SAVE_EXCEPTION_GPRS
    mov rax, [rsp + 136]
    test al, 3
    jz .pf_kernel_gs
    swapgs
.pf_kernel_gs:
    mov rdi, rsp
    call page_fault_dispatch
    mov rax, [rsp + 136]
    test al, 3
    jz .pf_restore_gs
    swapgs
.pf_restore_gs:
    RESTORE_EXCEPTION_GPRS
    add rsp, 8
    iretq

EXCEPTION_STUB_NOERROR x87, 16
EXCEPTION_STUB_ERROR alignment_check, 17
EXCEPTION_STUB_ERROR machine_check, 18
EXCEPTION_STUB_NOERROR simd, 19
EXCEPTION_STUB_NOERROR virtualization, 20
EXCEPTION_STUB_ERROR control_protection, 21

irq1_stub:
    cld
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    call irq1_dispatch
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

irq12_stub:
    cld
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    call irq12_dispatch
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

irq9_stub:
    cld
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    call irq9_dispatch
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

irq_timer_stub:
    cld
    ; A hardware IRQ does not perform SWAPGS. Switch to the kernel GS area
    ; before C code; the saved CS is wrapper-frame element 16.
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rax, [rsp + 128]
    test al, 3
    jz .timer_kernel_gs
    swapgs
.timer_kernel_gs:
    ; Wrapper frame at RSP: r15..rax, followed by the CPU IRET frame.
    ; C may select another wrapper frame; RAX is the returned frame pointer.
    mov rdi, rsp
    call irq_timer_dispatch
    mov rsp, rax
    mov rcx, [rsp + 128]
    test cl, 3
    jz .timer_return_kernel_gs
    swapgs
.timer_return_kernel_gs:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq


; Linux x86_64 SYSCALL entry. KERNEL_GS_BASE points to:
;   [0] kernel stack top
;   [8] saved user RSP
; The frame layout exactly matches syscall_frame_t in include/npk/syscall.h.
syscall_entry:
    swapgs
    cli                         ; never preempt while a syscall frame is active
    mov [gs:8], rsp
    mov rsp, [gs:0]
    cld

    push qword 0x1b             ; user SS
    push qword [gs:8]           ; user RSP
    push r11                    ; user RFLAGS
    push qword 0x23             ; user CS
    push rcx                    ; user RIP
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    mov rdi, rsp
    call syscall_dispatch_asm
    mov [rsp], rax              ; syscall return value becomes frame->rax

    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    ; The remaining five qwords are the exact IRET frame:
    ; RIP, CS, RFLAGS, user RSP, SS. Restore it directly instead of
    ; SYSRET so a normal syscall return and a demand-fault return share
    ; one validated privilege-transition path.
    swapgs
    iretq

; void context_switch(cpu_context_t *old, const cpu_context_t *new)
; cpu_context_t offsets: rip=0, rsp=8, rflags=16, rax=24, rbx=32,
; rcx=40, rdx=48, rsi=56, rdi=64, rbp=72, r8=80, r9=88,
; r10=96, r11=104, r12=112, r13=120, r14=128, r15=136, cr3=144.
; The target context returns through its saved RIP on its saved stack.
context_switch:
    mov [rdi + 8], rsp
    mov rax, [rsp]
    mov [rdi + 0], rax
    pushfq
    pop rax
    mov [rdi + 16], rax
    mov [rdi + 32], rbx
    mov [rdi + 72], rbp
    mov [rdi + 112], r12
    mov [rdi + 120], r13
    mov [rdi + 128], r14
    mov [rdi + 136], r15

    mov rax, [rsi + 144]
    test rax, rax
    jz .load_context
    mov r8, cr3
    and r8, -4096
    cmp rax, r8
    je .load_context
    mov cr3, rax
.load_context:
    mov rsp, [rsi + 8]
    mov rbx, [rsi + 32]
    mov rbp, [rsi + 72]
    mov r12, [rsi + 112]
    mov r13, [rsi + 120]
    mov r14, [rsi + 128]
    mov r15, [rsi + 136]
    mov rax, [rsi + 16]
    push rax
    popfq
    ret

; Restore a complete wrapper frame selected by scheduler_preempt/scheduler_yield.
; RDI points to r15..rax followed by a CPU IRET frame.
restore_interrupt_frame:
    mov rsp, rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

; Restore a raw SYSCALL frame while GS_BASE still points at the kernel CPU
; area. SYSCALL entry performed SWAPGS, unlike a hardware IRQ from user mode.
restore_user_frame_from_kernel:
    swapgs
    jmp restore_interrupt_frame

; Restore a kernel-only preemption frame. The frame contains fifteen GPRs,
; followed by RIP, CS, and RFLAGS. Kernel-to-kernel transfer uses RET rather
; than IRETQ because no privilege-level stack switch is required.
restore_kernel_frame:
    push qword [rdi + 136]
    popfq
    mov rsp, rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; Enter a validated user image. RDI=user RIP, RSI=user RSP.
; The caller must have mapped both addresses with user permissions.
enter_user_mode:
    cli
    ; Kernel runs with GS_BASE=npk_cpu0 and KERNEL_GS_BASE=user GS.
    ; Swap before IRET so user mode observes its own GS base.
    swapgs
    push qword 0x1b             ; user SS
    push rsi                    ; user RSP
    push qword 0x202            ; IF=1, reserved bit set
    push qword 0x23             ; user CS
    push rdi                    ; user RIP
    iretq

fault_hang:
    cli
    hlt
    jmp fault_hang

SECTION .bss
ALIGN 16
stack_bottom:
    RESB 65536
stack_top:
