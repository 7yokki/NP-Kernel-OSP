# NPKernel

NPKernel (**No Problem Kernel**), gelecekte geliştirilecek **NPOS** işletim sisteminin x86_64 çekirdek tabanıdır. Proje monolitik fakat modüler bir çekirdek düzenini izler; yaklaşık yüzde 20 assembly ve yüzde 60–80 C hedefiyle, donanıma yakın çalışma ile okunabilir C alt sistemleri arasında dengeli bir yapı kurar. Çekirdek Linux x86_64 syscall numaralarını ve register çağrı sözleşmesini kullanır; ancak bu durum tek başına tüm Linux kullanıcı alanı ikili dosyalarıyla uyumluluk anlamına gelmez. NPKernel, tam Linux uyumluluğu iddiasında bulunmadan, güvenli bir userspace temelini ve NPOS’a özgü genişletme noktalarını sağlar. Güncel kaynakta `rt_sigaction` yalnızca kayıt tutmakla sınırlı değildir: temel signal delivery, `kill`/`tgkill`, blocked/pending maskesi ve `rt_sigreturn` uygulanmıştır.

> **Tasarım sınırı:** NPKernel pencere yöneticisi, masaüstü, sprite, mouse imleci veya başka bir GUI katmanı çizmez. Kernel yalnızca güvenli bir framebuffer handoff/lease ve yapılandırılmış input temeli sağlar; compositor ve GUI userspace sorumluluğudur.

## Özellik durumu

| Alt sistem | Durum | Güncel kapsam |
|---|---:|---|
| Boot | Çalışıyor | Limine ile BIOS CD ve UEFI CD boot; higher-half ELF yükleme |
| GOP/TTY | Çalışıyor | 1920×1080×32 hedefi, kalın 10×16 bitmap font, 2× hücre ölçeği, satır kaydırma; kontrollü framebuffer info/claim/map/release ABI’si |
| VGA ve log | Çalışıyor | VGA text fallback, GOP console, COM1 seri log, panic handler |
| GDT/IDT/TSS | Çalışıyor | Ring 0/Ring 3 descriptor’ları, TSS/RSP0, IST, 256 IDT gate’i |
| Kesme ve timer | Çalışıyor | PIC remap, IRQ1 keyboard, IRQ12 PS/2 mouse, IRQ0 PIT 100 Hz, tam GPR preemption frame’i |
| PMM/VMM | Çalışıyor | Refcount’lu 4 KiB PMM, root-aware 4-level paging, user-range ve copyin/copyout |
| Kernel heap | Çalışıyor | `kmalloc`, `kcalloc`, `kfree`; geçersiz ve double-free kontrolleri |
| ELF64 | Çalışıyor | ET_EXEC/ET_DYN, bounds/overlap doğrulama, W^X, NX, rollback; bounded `PT_INTERP`/`PT_DYNAMIC` interpreter handoff ve auxv |
| Process/scheduler | Çalışıyor | PID/TID/TGID, kernel stack, gerçek context switch, timer tabanlı BSP preemption; bounded `clone` thread-group subset’i ve `exit_group` |
| VMA/mmap | Çalışıyor | Anonymous/file-backed `mmap`, immutable initramfs lazy faults, bounded shared-memory/device mapping, `munmap`, `mprotect`, `brk`, stack growth ve W^X |
| Fork/COW | Çalışıyor | `fork`, low-half address-space clone, copy-on-write page resolution |
| Exit/wait | Çalışıyor | Zombie süreç, parent wakeup, `wait4`/waitpid yolu, address-space/VMA/FD/stack cleanup |
| VFS/initramfs | Çalışıyor | CPIO `newc`, bounded directory hierarchy ve canonical absolute paths, refcount’lu open handles, paylaşımlı offset, pipe ring buffer, metadata/directory syscalls, bounded `poll`/`ppoll`/level-triggered `epoll` |
| Persistent NPKFS | Bounded | ATA PIO fixed-region `/persist`; create/read/write/truncate/append, close, `fsync`/`fdatasync`/`sync` ve readback metadata |
| `/proc` | Temel | Process, thread ve memory bilgisi için pseudo-filesystem yolları |
| ATA | Temel | Primary ATA PIO probe/read/write yolu; Q35’te cihaz yokluğu kontrollü ele alınır |
| SMP | Bring-up | Limine MP response, bounded CPU discovery ve QEMU `-smp 2` AP online; AP scheduler migration’ı henüz etkin değil |
| Keyboard/input | Çalışıyor | PS/2 set-1 Türkçe Q text queue, structured key press/release events, IRQ12 PS/2 mouse REL/button events |
| PCI | Temel | PCI config mechanism #1 ile bounded bus/device/function enumeration; vendor/class/header/BAR metadata | 
| ACPI güç | Çalışıyor | RSDT/XSDT, FADT/DSDT `_S5_`, MADT/IOAPIC SCI, PM1 soft-off |
| Linux ABI | Kademeli | Desteklenmeyen syscall’lar `-ENOSYS`; desteklenen yollar aşağıdaki tabloda listelenir |

### Uygulanan syscall yolları

Linux x86_64 ABI’de syscall numarası `RAX`, argümanlar ise sırasıyla `RDI, RSI, RDX, R10, R8, R9` üzerinden alınır. NPKernel’in dispatcher’ı bu düzeni korur ve kullanıcı dönüşünü güvenli bir `IRETQ` frame’i üzerinden yapar. [1] [2]

| Syscall ailesi | Uygulanan yollar |
|---|---|
| Dosya ve metadata | `read`, `write`, `open`, `close`, `fstat`, `stat`, `lseek`, `getdents64`, `getcwd`, `readlink`; canonical absolute initramfs paths, refcount’lu logical fd slotları |
| Process/IPC | `getpid`, `gettid`, `fork`, `execve`, `exit`, `exit_group`, `set_tid_address`, `wait4`; proje waitpid alias’ı; bounded `clone`; NPK shared-memory object ve parent/child descriptor transfer extension’ları |
| Bellek ve TLS | `mmap`, `munmap`, `mprotect`, `brk`, `arch_prctl`; lazy allocation, file-backed/private/shared/device mapping, demand fault, COW, W^X ve per-thread FS/GS base |
| Vectored/runtime I/O | `readv`, `writev`, `pipe`, `dup`, `dup2`, `fcntl`, `sched_yield`, `poll`, `ppoll`, `epoll`, TTY `ioctl`; regular file, pipe ve TTY readiness kapsamı |
| Sinyal ve fault yolu | `rt_sigaction`, `rt_sigprocmask`, `kill`, `tgkill`, `rt_sigqueueinfo`, `rt_tgsigqueueinfo`, bounded `siginfo_t`/`si_value` queue, user fault delivery, `SA_SIGINFO` frame ve `rt_sigreturn` |
| Zaman ve sistem | `clock_gettime`, `nanosleep`, `uname`, `/proc` erişim yolları; bounded `futex` (`WAIT`/`WAKE`, timeout, EINTR) ve `openat`/`fsync`/`fdatasync`/`sync` |
| Display/input/security | Canonical/user-range doğrulaması, W^X, ring ayrımı, ayrı kernel stack, copyin/copyout, single-owner framebuffer lease ve arbitrary physical map reddi |

`mmap` çağrısı `addr, length, prot, flags, fd, offset` sözleşmesini izler. Anonymous mapping önce yalnızca VMA olarak rezerve edilir; fiziksel sayfa ilk erişimde page-fault handler tarafından oluşturulur. `mprotect` VMA metadata’sını bölge sınırlarında böler, mevcut PTE izinlerini ve TLB’yi günceller ve W^X’i korur. `fork` sırasında writable kullanıcı sayfaları parent ve child arasında COW olarak paylaşılır. Bir write fault geldiğinde yalnızca ilgili sayfa kopyalanır.

Pipe uçları process-local logical fd slotlarına kurulur; `fork`, `dup`, `dup2` ve `fcntl(F_DUPFD*)` aynı VFS open-handle’ını refcount ile paylaşır. Pipe backend’i bounded, nonblocking ring-buffer semantiğine sahiptir; boş pipe `-EAGAIN`, okuyucusu kalmayan pipe’a yazma `-EPIPE` döndürür. Descriptor flags içinde `FD_CLOEXEC` tutulur. `poll`/`ppoll` ve level-triggered `epoll` regular file, pipe ve TTY readiness’ini bounded tablolarla sunar; blocking wait queue, `select` ve edge-triggered epoll sonraki aşamadır. NPK shared object fiziksel sayfaları `MAP_SHARED` ile process’ler arasında görünür kılar; descriptor transfer queue parent/child ilişkisiyle sınırlıdır. Display lease arbitrary fiziksel adres kabul etmez; kernel-controlled GOP range map edilir ve release/exit’te revoke edilir. Structured input keyboard/mouse eventleri aynı bounded queue’da tutulur. `/persist` üzerindeki NPKFS fixed-region metadata/data alanları ATA PIO ile close veya `fsync` sırasında commit edilir; çift metadata slotu, checksum, generation ve transaction marker ile sınırlı metadata recovery vardır; data checksum, journaling ve gerçek güç-kesintisi crash recovery yoktur.

## Süreç yaşam döngüsü

Her userspace süreci kendisine ait bir CR3/address-space root, VMA listesi, kernel stack’i ve FD sahipliğiyle çalışır. `exit` süreci zombie durumuna geçirir; parent bekliyorsa uyandırılır. `wait4` çıkış durumunu kullanıcı belleğine kopyaladıktan sonra child’ın VMA listesi, page table’ları, açık FD’leri ve kernel stack’i güvenli sırayla serbest bırakılır. Parent beklemeden önce runnable child varsa scheduler başka bir thread’e geçer; blocked parent’ın syscall frame’i korunur ve wakeup sırasında dönüş değeri bu frame’e yazılır.

Kullanıcı kaynaklı CPU exception’ları çekirdeği doğrudan panic’e sürüklemez. Geçerli lazy allocation, stack growth veya COW koşulları çözümlenir; çözülemeyen kullanıcı fault’u ilgili Linux sinyaline çevrilir, kayıtlı handler varsa user signal frame üzerinden çalıştırılır, yoksa yalnızca ilgili süreç hata koduyla sonlandırılır. Kernel kaynaklı fatal fault’lar ise panic yoluna gider.

## Audit raporundaki beş düzeltme

Güncel sürümde audit raporunda belirtilen beş kernel-side sorun giderilmiştir. **Exception isolation** ile ring-3 fault’ları kernel panic yerine process termination veya signal delivery yoluna ayrılır. **Signal delivery** artık `rt_sigaction` kaydını gerçekten handler’a yönlendirir; minimal user stack frame, restorer doğrulaması ve `rt_sigreturn` ile özgün syscall/IRET frame’i geri yüklenir. **execve** başarı yolundaki erişilebilir sahte `-EFAULT` dönüşü `__builtin_unreachable()` ile kaldırılmıştır. **sys_read** dosya/VFS okumalarında 4096-byte parçalarla istenen `count` değerine kadar devam eder; terminalin satır-fragment davranışı korunur. **getcwd** ise Linux x86_64 ABI’ye uygun olarak user buffer adresini değil, kopyalanan yolun byte sayısını döndürür ve küçük buffer için `-ERANGE` verir.

Bu düzeltmeler; `UD2` sonrası kernel’in çalışmaya devam ettiği fault-isolation testi, SIGILL handler’ın çağrılıp `exit(0)` ile tamamlandığı signal testi, temiz Clang derlemesi ve üretim ayarlı BIOS QEMU boot testiyle doğrulanmıştır. Ayrıntılı kayıt [`docs/verification.md`](docs/verification.md) dosyasındadır. ABI çerçevesi x86_64 giriş kuralları ve ELF/System V sözleşmeleriyle birlikte değerlendirilmiştir [1] [2].

## Dizin yapısı

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
├── user/               # ELF64 smoke programs
├── tools/              # Initramfs and font build helpers
├── docs/               # Design and verification records
└── build/              # Generated ELF, ISO, symbols and test captures
```

## Derleme ve çalıştırma

Ubuntu/Debian üzerinde `gcc`, `clang`, `lld`, `nasm`, `qemu-system-x86`, `xorriso`, `mtools`, `git`, `make` ve `binutils` paketleri gereklidir. Limine v9.x-binary build sırasında `build/limine` altına alınır. Initramfs üreticisi proje kökünü script konumundan bulur ve Makefile hedef çıktısını açıkça geçirir; bu nedenle arşiv başka bir dizine çıkarıldığında da build yolu `/home/ubuntu/npkernel` gibi sabit bir konuma bağlı değildir.

```bash
cd npkernel
make clean
make -j2 all disk
make check
make run
make run-uefi
make debug
```

`make run` SeaBIOS/legacy-PC yolunu, `make run-uefi` ise OVMF UEFI yolunu kullanır. OVMF yolu dağıtıma göre değişebilir; Makefile varsayılan olarak `/usr/share/OVMF/OVMF_CODE_4M.fd` dosyasını arar. QEMU seri çıktısı kernel loglarının en güvenilir doğrulama kanalıdır. Varsayılan build’de `NPK_ENABLE_USER_DEMO=0` olduğu için ring-3 smoke programı boot sırasında otomatik çalıştırılmaz. Geliştirme sırasında bu makro geçici olarak `1` yapılabilir; release build’inden önce tekrar `0` bırakılmalıdır.

## Mimari ve ABI kararları

Kernel C kaynakları `--target=x86_64-unknown-none-elf`, `-ffreestanding`, `-fno-pie`, `-mno-red-zone`, `-fno-stack-protector`, `-mcmodel=kernel` ve SSE/MMX kapalı seçenekleriyle derlenir. Kernel interrupt’ları SysV red zone’ı güvenli biçimde korumadığından red zone kullanılmaz. Assembly NASM Intel syntax kullanır; `_start` long-mode girişinden sonra bootstrap stack kurar.

GDT’de ring-0 ve ring-3 code/data selector’ları ayrıdır. TSS, kernel stack geçişi ve IST stack’lerini sağlar. `SYSCALL` girişi `SWAPGS` ile kernel stack’e geçer; dönüşte frame, register’lar ve privilege transition `IRETQ` ile geri yüklenir. PIT wrapper’ı tüm 15 GPR’ı saklayarak scheduler’a CPU’nun oluşturduğu frame’i verir; scheduler timer frame’i ile syscall frame’ini birbirine karıştırmaz.

VMM yeni kullanıcı address-space’lerinde yalnızca kernel’in upper-half PML4 dallarını miras alır. Kullanıcı low-half mapping’leri özel page-table dallarında oluşturulur; mevcut supervisor intermediate entries sonradan `VM_USER` yapılmaz. Bu düzen page-table izinlerinin kullanıcı tarafından yukarı doğru genişletilmesini önler.

## Geliştirilme notları

Bu proje tamamen AI desteği ile geliştirilmiş ve insan elinin sadece planlamasını yaptığı bir işletim sistemi çekirdeğidir. Proje [Manus.AI](https://manus.im) adlı agent tarafından düzenli olarak geliştirilmektedir.

## Lisans

NPKernel, [MIT License](LICENSE) altında yayımlanır. 7YokKi tarafından yürütülmekte ve No PRoblem adına paylaşılmaktadır.

## Referanslar

[1]: https://www.kernel.org/doc/html/latest/arch/x86/entry_64.html "Linux x86_64 entry documentation"
[2]: https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html "System V ABI ELF specification"
[3]: https://github.com/Limine-Bootloader/Limine/blob/v12.x/CONFIG.md "Limine configuration documentation"
[4]: https://uefi.org/specifications "UEFI specifications"
[5]: https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/04_ACPI_Hardware_Specification/ACPI_Hardware_Specification.html "ACPI Hardware Specification"
