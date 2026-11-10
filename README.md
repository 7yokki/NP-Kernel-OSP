# NPKernel

**NPKernel** (*No Problem Kernel*) is a small x86_64 monolithic kernel written in C17 and NASM. It is the kernel foundation of **NPOS**, a future operating system. It boots through Limine on BIOS and UEFI, runs isolated ring-3 processes, and reuses the Linux x86_64 syscall numbers and register convention for a bounded subset of the Linux ABI.

It is a learning-grade, security-minded kernel: a few things work end to end, many things are deliberately bounded, and anything unsupported fails with an error (`-ENOSYS` or a matching errno) instead of pretending to succeed. It is **not** a Linux replacement, and a matching syscall number does not imply matching Linux behavior.

## At a glance

| | |
|---|---|
| Architecture | x86_64, higher-half kernel, monolithic with modular subsystems |
| Boot | Limine v9.x, BIOS and UEFI, 1024×768 GOP framebuffer |
| Userspace | Isolated address spaces, `fork`/COW, `execve`, `wait4`, bounded threads (`clone`), signals, futex |
| Default program | `/bin/bash.elf`, a small freestanding shell in ring 3 (not GNU Bash) |
| Storage | Read-only CPIO initramfs plus a small persistent NPKFS on ATA PIO under `/persist` |
| Input | PS/2 (Turkish Q) and a USB HID boot keyboard through an xHCI driver |
| Size | about 10.8k lines of kernel C, 1.3k lines of headers, 0.7k lines of assembly, 1.5k lines of userspace C |
| Verified on | QEMU only. No physical hardware has been tested. |
| License | MIT |

**What works today:** the kernel boots, runs user programs in separate address spaces with W^X and NX enforced, isolates user faults from the kernel, and gives you an interactive shell on the framebuffer. You can type on a USB or PS/2 keyboard, run ELF programs as child processes in the foreground or background, run `bash -c` and script files, and write files that survive a reboot on a QEMU IDE disk.

**What does not exist yet:** a per-CPU SMP scheduler, a network or audio stack, AHCI/NVMe, USB hubs and hotplug, Unix sockets, shell pipes and redirection, full pthread and glibc/musl compatibility, and crash-consistent file data. See [Known limitations](#known-limitations).

> **Design boundary:** the kernel draws no window manager, desktop or cursor. It provides a single-owner framebuffer lease and a structured input queue; a compositor or GUI belongs in userspace.

## Feature status

| Subsystem | Status | Scope |
|---|---|---|
| Boot | Working | Limine BIOS and UEFI CD boot, higher-half ELF kernel, initramfs as a boot module |
| Console | Working | GOP terminal with a 10×16 bitmap font at 2× scale, VGA text fallback, COM1 serial log, panic handler |
| GDT/IDT/TSS | Working | Ring 0/3 descriptors, RSP0, IST stacks, 256 IDT gates |
| Interrupts/timer | Working | PIC remap, PIT at 100 Hz, PS/2 keyboard and mouse IRQs, full-GPR preemption frame |
| PMM/VMM | Working | Refcounted 4 KiB frames, 4-level paging, canonical and user-range checks, copyin/copyout |
| Kernel heap | Working | `kmalloc`/`kcalloc`/`kfree` with invalid-free and double-free detection |
| ELF64 loader | Working | `ET_EXEC`/`ET_DYN`, overflow and overlap validation, W^X, rollback on failure, bounded `PT_INTERP`/`PT_DYNAMIC` handoff with auxv |
| Processes/scheduler | Working | PID/TID/TGID, per-thread kernel stacks, preemptive round-robin on the BSP, zombies and `wait4` |
| Threads | Bounded | `clone` with `CLONE_VM\|THREAD\|SIGHAND` and a limited set of other flags, `exit_group`, `set_tid_address`, `futex` WAIT/WAKE |
| VMA/mmap | Working | Lazy anonymous and file-backed `mmap`, `mprotect`, `munmap`, `brk`, stack growth, COW, shared-memory and device mappings |
| Signals | Working | `rt_sigaction`, masks, `kill`/`tgkill`, `SA_SIGINFO` frames, `rt_sigreturn`, bounded realtime queue (`rt_sigqueueinfo`) |
| VFS | Working | CPIO `newc` initramfs, canonical absolute paths, refcounted handles with shared offsets, pipes, `poll`/`ppoll`/level-triggered `epoll` |
| NPKFS | Bounded | Fixed-region filesystem on ATA PIO, mounted at `/persist`: create, read, write, truncate, append, `fsync`/`sync` |
| `/proc` | Basic | Process, thread and memory information |
| PCI | Basic | Config-mechanism #1 enumeration with vendor, class and BAR metadata |
| Device Manager | Basic | Device and driver registry, category and model lookup from a `/ddc.json` catalog |
| USB | Bounded | xHCI (polling) root-port enumeration, descriptor parsing, HID boot keyboard with press/release events |
| ACPI | Working | RSDT/XSDT, FADT/DSDT `_S5_`, MADT/IOAPIC, SCI, PM1 soft power-off |
| SMP | Bring-up | Limine MP discovery and AP startup (tested with `-smp 2`). APs sit in a safe idle loop; all scheduling happens on the BSP. |
| Shell | Bounded | Ring-3 shell with child processes, background jobs, `-c`, scripts and built-ins (see below) |

## Technical overview

### Boot and architecture

The kernel is built with Clang and LLD for `x86_64-unknown-none-elf`: `-ffreestanding`, `-mcmodel=kernel`, `-mno-red-zone`, SSE/MMX/x87 disabled, `-Werror`. Interrupt handlers cannot preserve the SysV red zone, which is why it is off.

- **Entry:** `arch/x86_64/entry.asm` provides the `SYSCALL` entry (`SWAPGS`, switch to the per-thread kernel stack, return through `IRETQ`) and the PIT wrapper that saves all 15 GPRs. Timer frames and syscall frames are never mixed inside the scheduler.
- **Privilege separation:** separate ring-0 and ring-3 selectors, a TSS for RSP0 and IST stacks, and a separate kernel stack for every thread.
- **Boot handoff:** Limine requests (`boot/limine_requests.c`) supply the memory map, framebuffer, RSDP, MP info and the initramfs module.
- **Address spaces:** each new space inherits only the kernel's upper-half PML4 entries. User mappings live in private page-table branches, and supervisor intermediate entries are never later promoted to user-accessible, so user-driven permission widening is not possible through the page tables.

### Memory

Anonymous `mmap` first reserves only a VMA. The page fault handler allocates the physical page on first touch. `mprotect` splits VMAs at region boundaries, updates PTEs and the TLB, and keeps W^X. `fork` clones the low-half address space and marks writable pages copy-on-write, so only the written page is copied. Initramfs files can be mapped privately as lazy, immutable file-backed pages. Shared-memory objects (an NPK extension) make physical pages visible to several processes through `MAP_SHARED`.

### Processes, signals and faults

Each process owns its CR3, VMA list, FD table and kernel stack. `exit` leaves a zombie; `wait4` returns the status and then releases the VMAs, page tables, FDs and stack in a safe order. A parent blocked in `wait4` has its syscall frame preserved and gets the result written into it on wakeup.

User CPU exceptions never go straight to a kernel panic. Valid lazy allocation, stack growth and COW faults are resolved. Anything else becomes the matching Linux signal, is delivered through a user signal frame if a handler is registered, and otherwise terminates only that process. Faults raised inside the kernel still end in the panic path.

### Syscalls

Numbers and registers follow Linux x86_64 (`RAX`; `RDI, RSI, RDX, R10, R8, R9`). Pointers are validated and moved with copyin/copyout. Unsupported numbers return `-ENOSYS`. The full table with per-call limits is in [`docs/syscalls.md`](docs/syscalls.md); the ABI contract is in [`docs/user-abi.md`](docs/user-abi.md).

| Family | Implemented |
|---|---|
| Files and metadata | `read`, `write`, `open`, `openat` (`AT_FDCWD`), `close`, `stat`, `fstat`, `lseek`, `getdents64`, `getcwd`, `chdir`, `readlink`, `fsync`, `fdatasync`, `sync` |
| Process and threads | `getpid`, `gettid`, `fork`, `clone` (bounded), `execve`, `exit`, `exit_group`, `wait4`, `set_tid_address`, `futex` |
| Memory and TLS | `mmap`, `munmap`, `mprotect`, `brk`, `arch_prctl` (per-thread FS/GS base) |
| I/O multiplexing | `readv`, `writev`, `pipe`, `dup`, `dup2`, `fcntl`, `poll`, `ppoll`, `epoll_create1`/`ctl`/`wait`, `sched_yield`, TTY `ioctl` (`TCGETS`/`TCSETS*`) |
| Signals | `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `kill`, `tgkill`, `rt_sigqueueinfo`, `rt_tgsigqueueinfo` |
| Time and system | `clock_gettime`, `nanosleep`, `uname` |
| NPK extensions (0x400+) | Shared-memory objects, display info/claim/map/release, structured input read, parent/child FD transfer |

Pipes are bounded ring buffers: an empty pipe returns `-EAGAIN` and a write with no readers returns `-EPIPE`. Blocking pipe wait queues, `select` and edge-triggered epoll are not implemented.

### Filesystems and storage

The VFS has two backends behind one API: the read-only initramfs (CPIO `newc`, bounded directory hierarchy) and NPKFS under `/persist`. NPKFS is a small fixed-region filesystem on ATA PIO with 64 inodes and up to 128 direct sectors per file. Metadata has two slots, each with a CRC32, a generation counter and a transaction marker, so an interrupted metadata write falls back to the last valid slot. File data has no checksum and there is no journal, so this is **not** crash-consistent storage. The recovery tests corrupt on-disk metadata; they do not inject real power loss.

### Display, input and USB

The kernel gives userspace one framebuffer lease at a time. It maps only the kernel-controlled GOP range and revokes the lease on release or process exit. Arbitrary physical mappings are rejected. Keyboard and mouse events go into a bounded structured queue, and a separate text queue feeds the TTY.

The Device Manager registers PCI and USB devices and resolves their category (`bus + class + subclass + protocol`) and model (`VID:PID`) from `/ddc.json`, which is packed into the initramfs from `user/ddc.json`. The xHCI driver runs in polling mode. It addresses root-port devices, reads device and configuration descriptors, validates the HID interface, issues Configure Endpoint and `SET_CONFIGURATION`, selects boot protocol, and decodes interrupt-IN reports into key press and release events. USB HID and PS/2 share one Turkish Q layout (Shift number row, AltGr+E for `€`).

### Userspace shell

On a normal boot the kernel prepares the framebuffer as a terminal and starts `/bin/bash.elf` in ring 3. The shell is a small freestanding program built on the in-tree mini libc (`user/libc`). It is **not** GNU Bash.

- **Input:** the shell puts the TTY into raw mode, does its own line editing (including UTF-8 backspace), and blinks an underscore cursor.
- **Built-ins:** `help echo clear pwd cd ls cat head wc stat touch cp uname free ps whoami true false exit exec jobs wait`.
- **External programs:** any `/bin/<name>.elf` (or a path) runs as a child process through `fork`, `execve` and `waitpid`. A trailing `&` runs it in the background; `jobs` lists background children and `wait` waits for all of them.
- **Non-interactive modes:** `bash -c 'cmd'` and `bash script`. Scripts run one command per line, with `;` and `&` separators and `#` comments.
- **Writable paths:** only `/persist`.
- **Not supported:** pipelines, redirection, globbing, variables and expansion, `mkdir`/`rm`/`mv`, and job control.

## Verification

Every gate below passes on the last recorded run. All of this is **QEMU evidence only**; none of it is a physical-hardware result.

| Gate | What it covers |
|---|---|
| `make check` | Clean build, ELF headers and sections of the kernel |
| `make test-elf-corpus` | 38 adversarial ELF images against the loader validator (38/38 handled correctly) |
| `make test-device-manager` | Device Manager and DDC host suite (12/12) |
| `make test-usb-keyboard` | HID boot-report decoder host suite (19/19) |
| `make test-user-libc` | Host tests for the mini libc |
| `make test-usb-qemu` | q35 + `qemu-xhci` + `usb-kbd`: enumeration, driver bind, QMP key injection across a 16-TRB ring wrap |
| `make test-bash-qemu` | `-M pc -smp 2` with an IDE disk and USB keyboard: boots the ring-3 shell, types Turkish Q symbols, runs built-ins, `bash -c`, a script, a foreground and a background child, then `jobs`/`wait`, `/persist` file operations, a cursor blink check and a check that file names reached the disk image |

The raw logs are in [`docs/`](docs/); [`docs/verification.md`](docs/verification.md) is the full record. Some entries in that file predate the shell and still say child execution is `exec`-only. For current behavior, trust this README and `user/bash.c`.

## Build and run

On Ubuntu or Debian you need `clang`, `lld`, `nasm`, `qemu-system-x86`, `xorriso`, `mtools`, `git`, `make`, `binutils` and `python3`. The build clones Limine's `v9.x-binary` branch into `build/limine`, so it needs network access the first time.

```bash
make clean
make -j2 all disk       # kernel, initramfs, ISO and a 32 MiB raw disk image
make check
make run                # QEMU q35, serial on stdio
make run-uefi           # OVMF; override with OVMF_CODE=/path/to/OVMF_CODE.fd
make run-disk           # attaches the raw disk over IDE
make debug              # QEMU paused with a GDB stub on :1234

make test-elf-corpus test-device-manager test-usb-keyboard test-user-libc
make test-usb-qemu test-bash-qemu
```

Build switches: `NPK_ENABLE_USER_SHELL=0` stops the kernel from launching the shell; `NPK_ENABLE_USER_DEMO` (default `0`) enables the legacy smoke payload.

QEMU's serial output is the most reliable channel for kernel logs. **Persistence note:** the ATA PIO driver only finds a disk on the legacy `-M pc` machine with an IDE drive, which is what `test-bash-qemu` uses. On Q35 the primary IDE device is not presented, so the kernel reports `persistent NPKFS unavailable` and continues without `/persist`. This is a controlled warning, not a panic.

## Repository layout

```text
├── Makefile, linker.ld, limine.conf
├── arch/x86_64/       # entry.asm: SYSCALL entry, interrupt wrappers
├── boot/              # Limine requests and boot handoff
├── include/npk/       # Kernel-internal headers; user_abi.h is the user-facing ABI
├── src/
│   ├── arch/          # GDT, IDT, IRQ, PIT
│   ├── device/        # Device Manager and DDC catalog parser
│   ├── drivers/       # GOP, VGA, font, PS/2, PCI, ATA, USB core, xHCI, USB HID keyboard
│   ├── exec/          # ELF64 loader
│   ├── fs/            # VFS (initramfs + NPKFS) and /proc
│   ├── memory/        # PMM, VMM, VMAs/COW, heap
│   ├── proc/          # Processes, threads, scheduler, signals
│   ├── syscall/       # Syscall dispatcher
│   └── acpi.c, smp.c, console.c, log.c, panic.c, kernel.c
├── user/              # Shell, mini libc and headers, sample ELFs, ddc.json
├── initramfs/         # Files packed into the initramfs
├── tests/             # Host test suites and QEMU/QMP smoke tests
├── tools/             # Initramfs, font and adversarial-ELF generators
└── docs/              # Design notes, audits and raw verification logs
```

Design documents worth reading first: [`docs/syscalls.md`](docs/syscalls.md), [`docs/user-abi.md`](docs/user-abi.md), [`docs/kernel-completion-plan.md`](docs/kernel-completion-plan.md), [`docs/device-manager.md`](docs/device-manager.md) and [`docs/usb-xhci-design.md`](docs/usb-xhci-design.md). [`docs/security-audit.md`](docs/security-audit.md) is a historical snapshot, so its "missing" items are outdated. [`TODO.md`](TODO.md) tracks open work (written in Turkish), and [`agent.md`](agent.md) is a hand-off note for contributors.

## Known limitations

- **SMP:** application processors come online but only idle. There is no per-CPU run queue, IPI or APIC timer routing, and the global tables are not designed for parallel execution.
- **Threads and runtimes:** `clone` supports a bounded flag set. Robust futex lists, PI, requeue and bitset futexes are missing, so full pthread and Rust-std behavior is not guaranteed. A real musl or glibc static binary has not been smoke-tested yet.
- **Dynamic linking:** the kernel validates and maps `PT_INTERP` and sets up auxv. A userspace `ld.so` must do `DT_NEEDED`, symbol resolution and relocation, and none is shipped here.
- **Signals:** `rt_sigtimedwait` and full nested realtime frame semantics are missing.
- **I/O:** no blocking pipe wait queues, `select` or edge-triggered epoll. Time APIs are limited (`gettimeofday`, `times` and `clock_nanosleep` are absent).
- **Storage:** ATA PIO only, with no AHCI, NVMe or DMA/IOMMU. NPKFS is flat and fixed-size, has no directory tree, no data checksum or journal, and no real power-loss crash recovery.
- **USB:** the xHCI driver polls and supports HID boot keyboards only. There are no hubs, hotplug, unbind, multi-interface devices, SuperSpeed companion descriptors or report-protocol HID. USB mouse is not handled (PS/2 mouse is).
- **Hardware:** everything is tested under QEMU. Real-hardware behavior is unknown.
- **Networking, audio, Unix sockets, `eventfd`/`signalfd`:** not started.
- **Version string:** `uname` still reports release `0.2.0-dev`, which does not match the project version below.

## History

NPKernel started in 2025 and first appeared on GitHub as v0.4 in mid-2026, after earlier private versions such as v0.1. The current tree is an intermediate v0.8 that still receives small fixes ahead of a proper release.

Milestones: v0.3 userspace foundations (process lifecycle, COW, signals, five audit fixes), v0.4 kernel platform gate (PCI, PS/2 mouse, shared memory, display lease, persistent NPKFS, SMP bring-up), then USB HID input through xHCI, the Device Manager, and the ring-3 shell with child processes, background jobs, `-c` and script mode.

## License and credits

NPKernel is released under the [MIT License](LICENSE). It is maintained by 7YokKi and shared on behalf of No PRoblem.

Boot protocol: [Limine](https://github.com/limine-bootloader/limine). Specifications used: [Linux x86_64 entry](https://www.kernel.org/doc/html/latest/arch/x86/entry_64.html), [System V ABI / ELF](https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html), [UEFI](https://uefi.org/specifications), [ACPI](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/04_ACPI_Hardware_Specification/ACPI_Hardware_Specification.html), Intel xHCI 1.2 and USB-IF HID 1.11.
