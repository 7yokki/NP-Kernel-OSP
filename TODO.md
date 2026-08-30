# NPKernel TODO

Bu liste **kernel kapsamını** izler. Userspace programlarının kendi build sistemi, libc implementasyonu ve runtime ayrıntıları NPKernel’in dışında tutulur; ancak kernel’in gerekli Linux x86_64 ABI sözleşmeleri burada takip edilir.

## Tamamlanan çekirdek temelleri

- [x] Limine BIOS/UEFI boot ve UEFI/GOP tabanlı TTY konsolu.
- [x] 10×16 bold bitmap font, 2× hücre ölçeği ve Türkçe Q klavye.
- [x] PMM refcount, page-backed kernel heap, invalid pointer/double-free reddi.
- [x] 4-level VMM, kernel upper-half mirası, user-range/canonical pointer kontrolleri.
- [x] GDT/TSS/RSP0/IST, ring-0/ring-3 descriptor ayrımı ve güvenli `IRETQ` dönüşü.
- [x] IDT, PIC/PIT, keyboard IRQ, ACPI PM1 S5 kapatma ve SCI yönlendirmesi.
- [x] Linux x86_64 syscall frame’i, `SWAPGS`, per-thread kernel stack ve preemptive scheduler.
- [x] Güvenli ELF64 loader: ET_EXEC/ET_DYN, overflow/overlap denetimi, W^X/NX ve rollback.
- [x] Lazy anonymous `mmap`, page-fault materialization, stack growth, `munmap`, `mprotect` ve `brk`.
- [x] `fork` + address-space COW, write-fault çözümleme ve child frame hazırlığı.
- [x] `execve`, argc/argv/envp/auxv başlangıç stack’i ve kullanıcı kaynaklı #PF/#GP termination.
- [x] Zombie süreç, parent wakeup, `wait4`/`waitpid`, exit status ve VMA/page-table/FD/stack cleanup.
- [x] ATA PIO temeli, initramfs CPIO VFS ve `/proc` pseudo-filesystem.
- [x] Refcount’lu VFS open-handle’ları, process-local fd slotları, paylaşımlı offset ve fork fd mirası.
- [x] Bounded nonblocking pipe ring buffer, `dup`, `dup2`, `fcntl(F_DUPFD*)` ve `FD_CLOEXEC` metadata.
- [x] Immutable initramfs file-backed `mmap`, VMA backing refcount/split/clone cleanup ve `MAP_PRIVATE`/W^X policy.
- [x] Bounded anonymous shared-memory object, `MAP_SHARED` physical-page mapping ve parent/child descriptor transfer queue.
- [x] GOP framebuffer info/claim/map/release lease’i; arbitrary physical map reddi; PS/2 structured keyboard/mouse input event queue.
- [x] Bounded initramfs directory hierarchy, canonical absolute path normalization ve child `getdents64` entries.
- [x] Bounded PCI configuration mechanism #1 enumeration; device activation/driver binding yapılmaz.

## Tamamlanan Linux x86_64 ABI kapsamı

- [x] `arch_prctl` (158): `ARCH_SET_FS`/`ARCH_GET_FS` ile per-thread TLS base; `ARCH_SET_GS`/`ARCH_GET_GS` için kullanıcı GS base saklama ve scheduler aktivasyonu.
- [x] `sched_yield` (24): mevcut scheduler yield yoluna bağlı.
- [x] `readv` (19) ve `writev` (20): güvenli iovec copyin/copyout; stdout/stderr ve mevcut VFS read yolları ile sınırlı.
- [x] `rt_sigaction` (13): action kayıtları process başına doğrulanarak saklanıyor, fork’ta kopyalanıyor ve fault/kill delivery sırasında kullanılıyor.
- [x] Sinyal teslimi: pending bitset, blocked maskesi, user-frame injection, default action, handler dönüşü ve `rt_sigreturn`.
- [x] `rt_sigprocmask` (14), `rt_sigreturn` (15), `kill` (62) ve `tgkill` (234) için temel ABI yolları.
- [x] `rt_sigqueueinfo` (129) ve `rt_tgsigqueueinfo` (297): bounded `siginfo_t`/`si_value` payload queue, `SA_SIGINFO` frame ve `EAGAIN` overflow.
- [ ] `rt_sigtimedwait`/sigwaitinfo ve tam nested realtime signal-frame semantiği.
- [ ] Gerçek derlenmiş musl/glibc statik programıyla TLS/CRT/IO smoke testi.

## Gerçek kalan ABI ve kernel işleri

- [x] Bounded `futex` (202): address-space anahtarlı `FUTEX_WAIT`/`FUTEX_WAKE`, timeout, signal interruption ve `clear_child_tid` wake yolu.
- [x] Bounded thread-group/`clone` çekirdeği: TID/TGID alanları, `CLONE_VM|CLONE_THREAD|CLONE_SIGHAND`, sınırlı shared FS/files flag’leri, `set_tid_address`, `exit_group`, clear-child-tid ve group-wide exit/signal yolları.
- [ ] Tam pthread/Rust runtime semantiği: robust-list, PI/requeue/bitset futexleri, cancellation/join ayrıntıları ve tüm clone flag ailesi.
- [x] Bounded `poll`/`ppoll`/`epoll`: copyin/copyout, regular/pipe/TTY readiness, timeout ve refcount-safe epoll watch cleanup.
- [ ] Blocking pipe wait queue’ları, `select`, edge-triggered epoll ve terminal job control.
- [x] `openat` (AT_FDCWD), `fsync`, `fdatasync` ve `sync`; pathname/cwd kapsamı hâlâ bounded.
- [ ] `newfstatat`, genel symlink/readlink nesneleri, cwd/relative resolution, mount/permission namespace ve geniş `ioctl` modeli. `/proc/self/exe` readlink ve temel TTY ioctl şu an vardır.
- [ ] `gettimeofday`, `times`, `clock_nanosleep` ve daha eksiksiz saat/uyku kesilme semantiği.
- [x] Dynamic ELF kernel handoff: bounded `PT_INTERP`/`PT_DYNAMIC` validation, interpreter path lookup/map, `AT_PHDR`/`AT_PHNUM`/`AT_PHENT`/`AT_BASE`/`AT_ENTRY` auxv ve rollback. `DT_NEEDED` graphı, sembol çözümleme ve relocation userspace `ld.so` sorumluluğudur.
- [x] ELF adversarial validator corpus: 38 vakalık host harness; header/overflow/segment/W^X/entry/interpreter traversal/embedded-NUL/dynamic metadata reddi.
- [x] ATA PIO write yolu ve bounded persistent NPKFS: `/persist`, fixed-region metadata/data, çift metadata slotu, checksum/generation/transaction marker, open flags, close/fsync/fdatasync/sync ve readback altyapısı.
- [x] Limine MP/AP bring-up ve bounded SMP discovery: QEMU `-smp 2` AP online; AP’ler güvenli idle loop’ta tutulur.
- [ ] SMP scheduler/per-CPU run queue, IPI/timer routing, DMA/IOMMU, AHCI/NVMe ve network stack.
- [ ] USB host/HID, hotplug, gerçek device namespace ve input routing; PS/2 mouse yalnızca üç-byte bounded backend’tir.
- [ ] Unix sockets, eventfd/signalfd, general capability transfer, network ve audio kernel foundations.
- [ ] Persistent directory tree ve data journaling/crash-consistent filesystem; NPKFS metadata recovery bounded kalır.

> `kmalloc/kfree` kernel içi heap içindir. Userspace `malloc`, process adres alanındaki VMA/page ownership ve libc TLS sözleşmelerini kullanır; kernel yalnızca gerekli ABI ve bellek güvenliğini sağlar.

> **Uyumluluk sınırı:** Kernel artık dynamic ELF için interpreter handoff sağlayabilir; ancak `DT_NEEDED`/sembol çözümleme/relocation userspace `ld.so` tarafından yapılmalıdır. Thread, futex, poll/epoll, shared memory, display/input, persistent storage, PCI/PS2 mouse, SMP ve realtime signal yolları belgelenen bounded subset’tir; tam pthread/Rust runtime, per-CPU SMP scheduler, USB/network/audio stack ve tüm Linux pathname/signal semantiği iddia edilmez.
