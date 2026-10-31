# NPKernel

NPKernel (**No Problem Kernel**) is the x86_64 kernel foundation of **NPOS**, an operating system to be developed in the future. The project follows a monolithic but modular kernel layout; with a target of roughly 20 percent assembly and 60–80 percent C, it strikes a balance between hardware-near code and readable C subsystems. The kernel uses the Linux x86_64 syscall numbers and register calling convention; however, this alone does not imply compatibility with all Linux userspace binaries. NPKernel provides a secure userspace foundation and NPOS-specific extension points without claiming full Linux compatibility. In the current source, `rt_sigaction` is not limited to merely recording registrations: basic signal delivery, `kill`/`tgkill`, blocked/pending masks and `rt_sigreturn` are implemented.

> **Design boundary:** NPKernel does not draw a window manager, desktop, sprites, a mouse cursor or any other GUI layer. The kernel only provides a secure framebuffer handoff/lease and a structured input foundation; the compositor and GUI are the responsibility of userspace.

> Note: README.md may fundamentally be inaccurate because it may be updated late

## Feature status

| Subsystem | Status | Current scope |
|---|---:|---|
| Boot | Working | BIOS CD and UEFI CD boot via Limine; higher-half ELF loading |
| GOP/TTY | Working | 1024×768×32 target, black userspace terminal surface, bold 10×16 bitmap font and 2× cell scale; kernel TTY canonical read, raw/poll character input for the shell, and a blinking underscore cursor |
| VGA and log | Working | VGA text fallback, GOP console, COM1 serial log, panic handler |
| GDT/IDT/TSS | Working | Ring 0/Ring 3 descriptors, TSS/RSP0, IST, 256 IDT gates |
| Interrupts and timer | Working | PIC remap, IRQ1 keyboard, IRQ12 PS/2 mouse, IRQ0 PIT 100 Hz, full-GPR preemption frame |
| PMM/VMM | Working | Refcounted 4 KiB PMM, root-aware 4-level paging, user-range checks and copyin/copyout |
| Kernel heap | Working | `kmalloc`, `kcalloc`, `kfree`; invalid-free and double-free checks |
| ELF64 | Working | ET_EXEC/ET_DYN, bounds/overlap validation, W^X, NX, rollback; bounded `PT_INTERP`/`PT_DYNAMIC` interpreter handoff and auxv |
| Process/scheduler | Working | PID/TID/TGID, kernel stack, real context switch, timer-based BSP preemption; bounded `clone` thread-group subset and `exit_group` |
| VMA/mmap | Working | Anonymous/file-backed `mmap`, immutable initramfs lazy faults, bounded shared-memory/device mapping, `munmap`, `mprotect`, `brk`, stack growth and W^X |
| Fork/COW | Working | `fork`, low-half address-space clone, copy-on-write page resolution |
| Exit/wait | Working | Zombie processes, parent wakeup, `wait4`/waitpid path, address-space/VMA/FD/stack cleanup |
| VFS/initramfs | Working | CPIO `newc`, bounded directory hierarchy and canonical absolute paths, refcounted open handles, shared offsets, pipe ring buffer, metadata/directory syscalls, bounded `poll`/`ppoll`/level-triggered `epoll` |
| Persistent NPKFS | Bounded | ATA PIO fixed-region `/persist`; create/read/write/truncate/append, close, `fsync`/`fdatasync`/`sync` and readback metadata |
| `/proc` | Basic | Pseudo-filesystem paths for process, thread and memory information |
| ATA | Basic | Primary ATA PIO probe/read/write path; absence of a device on Q35 is handled in a controlled manner |
| SMP | Bring-up | Limine MP response, bounded CPU discovery and AP online with QEMU `-smp 2`; AP scheduler migration is not yet enabled |
| Keyboard/input | Working | Backward-compatible PS/2 set-1 Turkish Q and USB HID boot keyboard; shifted number-row/punctuation, right-AltGr Euro, shared TTY text queue and structured key events; IRQ12 PS/2 mouse REL/button events |
| Userland shell | Bounded | Default `/bin/bash.elf` in ring 3; minimal freestanding libc, raw stdin/stdout line editor, underscore cursor with a roughly 2-second blink cycle and bounded Linux-like built-in commands; launching an ELF in the current shell process via `exec` |
| PCI | Basic | Bounded bus/device/function enumeration via PCI config mechanism #1; vendor/class/header/BAR metadata | 
| ACPI power | Working | RSDT/XSDT, FADT/DSDT `_S5_`, MADT/IOAPIC SCI, PM1 soft-off |
| Linux ABI | Incremental | Unsupported syscalls return `-ENOSYS`; supported paths are listed in the table below |

## Ring-3 shell

On a normal boot, the kernel prepares Limine's roughly **1024×768 GOP** framebuffer as a black terminal surface and starts `/bin/bash.elf` in ring 3. The shell uses in-project freestanding headers such as `stdio.h`, `unistd.h` and `string.h`, along with the small runtime in `user/libc`; it processes raw/poll character input on FD 0 with its own line editor, and FD 1/2 are stdout/stderr. The `_` cursor at the end of the prompt toggles between visible and hidden roughly once per second (a full blink cycle takes roughly two seconds); backspace and Turkish UTF-8 character echo are supported.

Supported built-in commands: `help`, `echo`, `clear`, `pwd`, `cd`, `ls`, `cat`, `uname`, `free`, `ps`, `whoami`, `true`, `false` and `exit`. On Turkish Q, the Shift+1–0 number/symbol layer, `*`, `?`, `+`, `/`, `=`, `_` and basic AltGr+E `€` output are mapped in both the USB HID and PS/2 text paths. When `exec <program>` succeeds, it replaces the current shell process with `/bin/<program>.elf` (or the given path); **it is not a general shell command runner that forks and waits for a child**. The command line provides limited quoting/backslash tokenization; GNU Bash compatibility, variable/parameter expansion, pipelines, redirection, globbing, job control and full Linux userspace compatibility are out of scope. The QEMU test sends a symbol command over USB HID and checks the cursor blink in idle screenshots; the evidence is in [`docs/qemu-bash-shell-smoke.png`](docs/qemu-bash-shell-smoke.png), [`docs/qemu-bash-turkish-q-symbols.png`](docs/qemu-bash-turkish-q-symbols.png) and [`docs/qemu-bash-shell-smoke.log`](docs/qemu-bash-shell-smoke.log).

### Implemented syscall paths

In the Linux x86_64 ABI, the syscall number is passed in `RAX`, and the arguments in `RDI, RSI, RDX, R10, R8, R9` respectively. NPKernel's dispatcher preserves this layout and performs the return to user mode through a secure `IRETQ` frame. [1] [2]

For the number, register arguments, a short description and bounded-scope details, see the [full syscall reference document](docs/syscalls.md).

| Syscall family | Implemented paths |
|---|---|
| File and metadata | `read`, `write`, `open`, `close`, `fstat`, `stat`, `lseek`, `getdents64`, `getcwd`, `chdir`, `readlink`; bounded cwd-relative initramfs paths, refcounted logical fd slots |
| Process/IPC | `getpid`, `gettid`, `fork`, `execve`, `exit`, `exit_group`, `set_tid_address`, `wait4`; project waitpid alias; bounded `clone`; NPK shared-memory object and parent/child descriptor transfer extensions |
| Memory and TLS | `mmap`, `munmap`, `mprotect`, `brk`, `arch_prctl`; lazy allocation, file-backed/private/shared/device mapping, demand fault, COW, W^X and per-thread FS/GS base |
| Vectored/runtime I/O | `readv`, `writev`, `pipe`, `dup`, `dup2`, `fcntl`, `sched_yield`, `poll`, `ppoll`, `epoll`, TTY `ioctl`; regular file, pipe and TTY readiness coverage |
| Signal and fault path | `rt_sigaction`, `rt_sigprocmask`, `kill`, `tgkill`, `rt_sigqueueinfo`, `rt_tgsigqueueinfo`, bounded `siginfo_t`/`si_value` queue, user fault delivery, `SA_SIGINFO` frame and `rt_sigreturn` |
| Time and system | `clock_gettime`, `nanosleep`, `uname`, `/proc` access paths; bounded `futex` (`WAIT`/`WAKE`, timeout, EINTR) and `openat`/`fsync`/`fdatasync`/`sync` |
| Display/input/security | Canonical/user-range validation, W^X, ring separation, separate kernel stack, copyin/copyout, single-owner framebuffer lease and rejection of arbitrary physical maps |

The `mmap` call follows the `addr, length, prot, flags, fd, offset` contract. An anonymous mapping is first reserved only as a VMA; the physical page is created by the page-fault handler on first access. `mprotect` splits VMA metadata at region boundaries, updates existing PTE permissions and the TLB, and preserves W^X. During `fork`, writable user pages are shared between parent and child as COW. When a write fault occurs, only the relevant page is copied.

Pipe ends are set up on process-local logical fd slots; `fork`, `dup`, `dup2` and `fcntl(F_DUPFD*)` share the same VFS open handle via refcounting. The pipe backend has bounded, nonblocking ring-buffer semantics; an empty pipe returns `-EAGAIN`, and writing to a pipe with no remaining readers returns `-EPIPE`. `FD_CLOEXEC` is kept in the descriptor flags. `poll`/`ppoll` and level-triggered `epoll` expose regular file, pipe and TTY readiness through bounded tables; blocking wait queues, `select` and edge-triggered epoll are a later stage. NPK shared objects make physical pages visible across processes via `MAP_SHARED`; the descriptor transfer queue is limited to the parent/child relationship. The display lease does not accept arbitrary physical addresses; the kernel-controlled GOP range is mapped and revoked on release/exit. Structured input keeps keyboard/mouse events in the same bounded queue. The NPKFS fixed-region metadata/data areas under `/persist` are committed via ATA PIO on close or `fsync`; bounded metadata recovery exists through dual metadata slots, checksum, generation and transaction marker; there is no data checksum, journaling or true power-loss crash recovery.

## Process lifecycle

Each userspace process runs with its own CR3/address-space root, VMA list, kernel stack and FD ownership. `exit` moves the process to the zombie state; if the parent is waiting, it is woken up. After `wait4` copies the exit status to user memory, the child's VMA list, page tables, open FDs and kernel stack are released in a safe order. If there is a runnable child before the parent waits, the scheduler switches to another thread; the blocked parent's syscall frame is preserved, and the return value is written to this frame upon wakeup.

User-originated CPU exceptions do not drive the kernel directly into a panic. Valid lazy allocation, stack growth or COW conditions are resolved; an unresolvable user fault is translated into the relevant Linux signal, executed via a user signal frame if a registered handler exists, otherwise only the relevant process is terminated with an error code. Fatal faults originating from the kernel go to the panic path.

## Five fixes from the audit report

In the current version, the five kernel-side issues identified in the audit report have been fixed. With **exception isolation**, ring-3 faults are routed to process termination or signal delivery instead of a kernel panic. **Signal delivery** now actually routes the `rt_sigaction` registration to the handler; the original syscall/IRET frame is restored via a minimal user stack frame, restorer validation and `rt_sigreturn`. The reachable bogus `-EFAULT` return on the **execve** success path has been removed with `__builtin_unreachable()`. **sys_read** continues in 4096-byte chunks on file/VFS reads up to the requested `count`; the terminal's line-fragment behavior is preserved. **getcwd**, in line with the Linux x86_64 ABI, returns the byte count of the copied path rather than the user buffer address, and returns `-ERANGE` for a small buffer.

These fixes were verified by a fault-isolation test in which the kernel keeps running after `UD2`, a signal test in which the SIGILL handler is invoked and completes with `exit(0)`, a clean Clang build and a production-configured BIOS QEMU boot test. The detailed record is in [`docs/verification.md`](docs/verification.md). The ABI framework was evaluated together with the x86_64 entry rules and the ELF/System V contracts [1] [2].

## Directory structure

```text
npkernel/
├── Makefile
├── README.md
├── LICENSE
├── TODO.md
├── linker.ld
├── limine.conf
├── include/npk/       # Kernel public interfaces
├── boot/               # Limine request structures and boot handoff
├── arch/x86_64/        # Entry, GDT, IDT and interrupt assembly/C
├── src/
│   ├── arch/           # PIC, IDT, GDT, timer and ACPI paths
│   ├── drivers/        # TTY framebuffer, VGA, keyboard, PS/2 mouse, PCI and font
│   ├── exec/           # Secure ELF64 loader
│   ├── fs/             # CPIO-backed VFS and /proc
│   ├── memory/         # PMM, VMM, VMAs and COW
│   ├── proc/           # Process, thread and scheduler implementation
│   ├── syscall/        # Linux x86_64 syscall dispatcher
│   ├── console.c
│   ├── log.c
│   ├── panic.c
│   └── kernel.c
├── user/               # bash.c, minimal freestanding libc and ELF64 programs
├── tools/              # Initramfs and font build helpers
├── docs/               # Design and verification records
└── build/              # Generated ELF, ISO, symbols and test captures
```
## History

The project was uploaded to GitHub only about 2 months ago (July or August 2026) as version v0.4, but an early v0.1 version actually exists as well, and the project began to be written around 2025. The current version, v0.8, is an intermediate version that continues to be updated with small bugfixes ahead of the full release.

## Build and run

On Ubuntu/Debian, the `gcc`, `clang`, `lld`, `nasm`, `qemu-system-x86`, `xorriso`, `mtools`, `git`, `make` and `binutils` packages are required. The Limine v9.x binary is placed under `build/limine` during the build. The initramfs generator locates the project root from the script location and passes the Makefile target output explicitly; therefore, the build path is not tied to a fixed location such as `/home/ubuntu/npkernel` when the archive is extracted to another directory.

```bash
cd npkernel
make clean
make -j2 all disk
make check
make run
make run-uefi
make debug
```

`make run` uses the SeaBIOS/legacy-PC path, while `make run-uefi` uses the OVMF UEFI path. The OVMF path may vary by distribution; the Makefile looks for `/usr/share/OVMF/OVMF_CODE_4M.fd` by default. The QEMU serial output is the most reliable verification channel for kernel logs. The default build starts `/bin/bash.elf` in ring 3 with `NPK_ENABLE_USER_SHELL=1`; only the legacy smoke payload, `NPK_ENABLE_USER_DEMO=0`, remains. To disable the shell, use `make NPK_ENABLE_USER_SHELL=0`. This shell is not GNU Bash or full Linux userspace compatibility; the supported command/syscall subset is defined below and in `user/bash.c`.

## Architecture and ABI decisions

The kernel C sources are compiled with the options `--target=x86_64-unknown-none-elf`, `-ffreestanding`, `-fno-pie`, `-mno-red-zone`, `-fno-stack-protector`, `-mcmodel=kernel` and SSE/MMX disabled. Since kernel interrupts do not safely preserve the SysV red zone, the red zone is not used. Assembly uses NASM Intel syntax; `_start` sets up the bootstrap stack after long-mode entry.

The GDT keeps separate ring-0 and ring-3 code/data selectors. The TSS provides kernel stack switching and IST stacks. The `SYSCALL` entry switches to the kernel stack with `SWAPGS`; on return, the frame, registers and privilege transition are restored with `IRETQ`. The PIT wrapper saves all 15 GPRs and gives the scheduler the CPU-generated frame; the scheduler does not mix the timer frame with the syscall frame.

For new user address spaces, the VMM inherits only the kernel's upper-half PML4 branches. User low-half mappings are created in private page-table branches; existing supervisor intermediate entries are not later made `VM_USER`. This layout prevents user-driven upward widening of page-table permissions.

## License

NPKernel is published under the [MIT License](LICENSE). It is run by 7YokKi and shared on behalf of No PRoblem.

## References

[1]: https://www.kernel.org/doc/html/latest/arch/x86/entry_64.html "Linux x86_64 entry documentation"
[2]: https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html "System V ABI ELF specification"
[3]: https://github.com/Limine-Bootloader/Limine/blob/v12.x/CONFIG.md "Limine configuration documentation"
[4]: https://uefi.org/specifications "UEFI specifications"
[5]: https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/04_ACPI_Hardware_Specification/ACPI_Hardware_Specification.html "ACPI Hardware Specification"
