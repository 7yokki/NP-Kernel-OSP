# Phase 3 implementation notes

The current implementation adds Linux-compatible mmap validation for anonymous mappings, fork dispatch, wait4/waitpid dispatch, blocked parent state, zombie wakeup, explicit-root status copyout, VMA/address-space teardown, and per-process FD cleanup. Temporary ring-3 entry and scheduler diagnostic logging has been reduced after BIOS smoke testing.

The next validation step is a clean build followed by BIOS/UEFI smoke tests. The user demo remains enabled temporarily for ring-3 exit verification and must be disabled before final packaging.

The scheduler uses the CPU-pushed CS privilege bits as the source of truth for timer frames. A ring-3 syscall that exits with no runnable successor leaves interrupts disabled while the non-returning halt path is entered, preventing a timer IRQ from attempting to restore a kernel-only syscall stack as an IRET frame.
