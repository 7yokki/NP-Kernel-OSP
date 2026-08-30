# Preemptive process and memory design

> **Historical design note.** This document records the pre-implementation design snapshot. It is not the current feature-status source of truth; consult `README.md`, `TODO.md`, `docs/verification.md`, and `docs/security-report-triage.md` for the implemented state.

## Verified baseline

NPKernel currently has a cooperative round-robin scheduler. `scheduler_tick()` only sets a global reschedule flag; the PIT IRQ wrapper restores the interrupted register set and returns with `iretq`, so no timer-driven context switch occurs while a thread is running. The current `context_switch` saves only the callee-saved subset plus `RSP`, `RIP`, `RFLAGS`, and `CR3`; it is suitable for cooperative C-call yield points but not for an interrupted user/kernel trap frame.

The user syscall path builds a synthetic `syscall_frame_t`, but the assembly return path restores the original user `RIP/RSP` from the assembly frame. A preemptive scheduler therefore needs a separate interrupt-frame switch path that can save and restore a complete CPU frame, including user-vs-kernel privilege state, rather than calling the cooperative `context_switch` directly from the PIT handler.

`vm_map_anonymous()` currently allocates and maps every page eagerly. VMAs do not record stack status, lazy allocation, or COW state. `page_fault_dispatch()` terminates every CPL3 page fault without inspecting the fault address, error bits, or VMA. `vmm_destroy_address_space()` frees all low-half page-table leaves but has no physical-page reference counting and no clone/COW operation.

The current VFS has a global descriptor array. `process_t.fds[]` is not populated or consulted. Regular initramfs descriptors are global handles with shared offsets and are not released by `vfs_close()`, while pseudo descriptors are globally allocated. Fork and process cleanup therefore require at least process-local descriptor installation plus a controlled clone/close hook.

`process_exit_current()` only flips `alive` and marks the current thread zombie. It does not destroy the address space, free the kernel stack, close descriptors, notify a parent, or retain a waitable zombie record. There is no parent PID, child list, wait condition, or wait syscall implementation.

## Selected implementation order

The implementation will proceed in small testable slices: (1) complete interrupt-frame capture and timer preemption on the single CPU; (2) add VMA-backed lazy anonymous mapping and a bounded downward-growing user stack; (3) add a reference-counted physical page layer and a private address-space clone with copy-on-write write-fault handling; (4) implement fork and in-place exec image replacement; (5) add parent/child zombie and wait4 behavior, cleanup, and process-local FD ownership; (6) update boot smoke tests, README, MIT license, and release notes.

The first preemptive milestone will use a dedicated scheduler interrupt frame stored on each thread. The PIT wrapper will save all GPRs and pass the exact CPU-created frame to C. If the interrupted frame is CPL3, the scheduler will copy the frame into the current thread record, select a READY thread, switch CR3/TSS.RSP0, and restore the selected frame before `iretq`. If no alternate thread exists, the wrapper returns normally. This avoids corrupting the cooperative `context_switch` contract.

Lazy anonymous VMAs will reserve address ranges without leaf PTEs. The page-fault handler will allocate a zero page only for a not-present user read/write fault inside a lazy anonymous VMA. A user stack fault is accepted only below the current stack bottom, within one guard-page growth step, above a fixed lower bound, and with a user write/read access. Protection faults, instruction-fetch faults on NX pages, noncanonical addresses, and accesses outside a VMA terminate the process with status 139.

For fork, page references will be tracked in PMM metadata. Parent and child will share present user pages, clear `VM_WRITE` in both mappings, and mark the VMA/page as COW in kernel metadata. A user write fault to a COW page allocates and copies a private page, updates the leaf writable mapping, decrements the old reference, and resumes. Read-only pages remain shared. Kernel upper-half mappings remain shared and are never included in the user clone.

Exec will build a complete replacement address space first. On success, it will atomically replace the current process root, VMAs, image/brk metadata, and user frame `RIP/RSP`; the old root is destroyed only after CR3 switches to the new root. Failed exec leaves the old image alive. Exit marks a process zombie, releases its address space and owned descriptors, wakes a waiting parent, and keeps only a small waitable process record until the parent reaps it. An orphan is reparented to the kernel process and can be reaped by the kernel reaper path.
