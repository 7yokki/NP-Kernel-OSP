Current diagnosis after adding VFS metadata and SysV-style initial stacks:

- VFS metadata smoke test passes: root getdents64 returns 0x90 bytes and root fstat returns 0.
- The new argv/envp stack builder compiles and is used by process_exec_image(); the hello kernel demo calls the zero-argument wrapper.
- A ring-3 opt-in boot initially faulted in draw_codepoint at 0xb8000 after switching to the user CR3. VGA/console accesses were moved to the HHDM alias.
- A temporary attempt to map the framebuffer/VGA identity range into each user root caused an ELF -ENOMEM regression; that mapping was removed. ensure_table was relaxed to permit mixed supervisor/user page-table branches, but the correct production solution is shared HHDM access, not identity mapping.
- After HHDM conversion, the guest reaches process_launch_user and then becomes silent without the expected syscall 60/process exit marker. The likely next issue is GS state at direct IRET: enter_user_mode currently executes IRETQ without SWAPGS, while syscall_entry begins with SWAPGS and expects KERNEL_GS_BASE to point to npk_cpu0. Verify GS base setup and user return sequencing before changing the initial-stack implementation.
- The current trace showed no fatal serial exception after the HHDM fix; the guest timed out, so inspect the SYSCALL/GS transition rather than assuming ELF or stack construction failure.

Verified assembly details:

The original bring-up contract intentionally left IA32_GS_BASE at zero and placed npk_cpu0 in IA32_KERNEL_GS_BASE. That historical arrangement is no longer used after per-thread TLS support was added. The production invariant is now GS_BASE=npk_cpu0 while running in the kernel and KERNEL_GS_BASE=the selected user GS base; the initial kernel-to-user path performs SWAPGS before IRETQ, and syscall_entry performs SWAPGS back to the kernel CPU-local area. `ARCH_SET_FS` writes IA32_FS_BASE, while ARCH_SET_GS updates the stored user GS base without exposing the kernel CPU-local pointer to ring 3. The frame layout remains internally consistent with syscall_frame_t, and returns use IRETQ rather than SYSRET.

The user stack builder returns an RSP around NPK_USER_STACK_TOP minus only a few dozen bytes for zero arguments, so process_create_user's check against the page immediately below that RSP should pass because the entire eight-page stack is mapped. The stack contents are not needed by hello.elf, which only executes mov rdi,0; mov rax,60; syscall. The current syscall 60 branch calls process_exit_current and then deliberately halts forever, so absence of a post-exit marker does not distinguish successful syscall entry from failure; a very early assembly marker and a scheduler handoff are required.

Final verified diagnosis and fixes:

The initial silent ring-3 symptom was not caused by the zero-argument stack layout or by an incorrect direct IRETQ GS state. A temporary marker placed before SWAPGS appeared in the serial stream, and the preserved register trace then showed syscall number 60. The first apparent failure after the marker was caused by preserving the diagnostic marker incorrectly: it overwrote RAX, changing exit(60) into syscall 83 and exposing a later SYSRET #GP. The marker was then made register-safe with temporary scratch storage, proving the real path.

The production fix makes syscall exit non-returning with respect to SYSRET. It marks the current process dead, invokes scheduler_yield(), and falls back to a ring-0 halt only when no runnable thread exists. The temporary markers were removed afterward. The relaxed ensure_table permission propagation was also reverted because user address spaces already have private low-half PML4 branches and inherit only upper-half kernel entries; widening an existing supervisor branch would violate the user/kernel page-table boundary.

A separate final BIOS smoke-test fault revealed that irq_timer_stub passed the current RSP as the C frame pointer before saving RDI. The wrapper now saves all fifteen general-purpose registers first and passes the CPU-created interrupt frame at rsp+120. After this correction, both BIOS/SeaBIOS and OVMF/Q35 default boots reached the cooperative scheduler smoke thread without page fault, general-protection fault, or panic markers.
