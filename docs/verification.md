# NPKernel doğrulama kaydı

Bu kayıt NPKernel’in dynamic ELF handoff, bounded thread-group, poll/epoll, persistent NPKFS, Limine MP/AP bring-up, realtime signal payload queue, preemptive scheduler, lazy VM, fork/COW, process cleanup ve wait4/waitpid çalışmalarından sonraki durumunu özetler. Testler x86_64 QEMU üzerinde, seri çıktı `/tmp` altındaki kayıt dosyalarına alınarak yürütülmüştür.

## Temiz build

Üretim kaynak ağacı için aşağıdaki komut başarıyla tamamlandı:

```bash
make clean
make -j2 all disk
```

Derleme Clang freestanding C17, NASM, `ld.lld`, `objcopy`, Limine v9.x-binary ve `xorriso` ile tamamlandı. `build/npkernel.elf`, `build/npkernel.iso`, disk imajı ve initramfs üretildi. Üretim ayarında `NPK_ENABLE_USER_DEMO=0` bırakıldı; böylece ring-3 payload’ı normal boot sırasında otomatik başlatılmıyor.

## BIOS/SeaBIOS smoke testi

Kullanılan yol:

```bash
timeout 10s qemu-system-x86_64 \
  -M pc -m 512M -cdrom build/npkernel.iso \
  -serial file:/tmp/npk-production-bios.log \
  -display none -no-reboot -no-shutdown
```

Test beklenen timeout ile sona erdi; kernel idle loop içinde çalışmayı sürdürdüğü için QEMU kendiliğinden kapanmıyor. Seri kayıtta Limine base revision, GOP framebuffer, HHDM, GDT/TSS, IDT, PIC, PIT 100 Hz, ACPI, PMM, VMM, keyboard, VFS, syscall dispatcher ve `NPKernel initialization complete` satırları görüldü. Üretim boot kaydında page fault, general-protection fault veya panic satırı bulunmadı.

## UEFI/OVMF smoke testi

Kullanılan firmware ile Q35 yolu:

```bash
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/npk-OVMF_VARS_4M.fd
timeout 15s qemu-system-x86_64 \
  -M q35 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/tmp/npk-OVMF_VARS_4M.fd \
  -cdrom build/npkernel.iso \
  -serial file:/tmp/npk-production-uefi.log \
  -display none -no-reboot -no-shutdown
```

OVMF, Limine UEFI executable’ını başlattı; kernel GOP framebuffer boyutunu, HHDM offset’ini ve `NPKernel initialization complete` kaydını üretti. Q35 yapılandırmasında primary IDE cihazı bulunmadığı için ATA probe kontrollü biçimde unavailable döndü; bu, UEFI boot doğrulamasını etkilemedi.

## Ring-3 hello ve syscall exit

Minimal `user/hello.asm` payload’ı `exit(0)` syscall’ına indirgenmiştir:

```asm
mov eax, 60
xor edi, edi
syscall
hlt
```

Geçici olarak `NPK_ENABLE_USER_DEMO=1` yapılarak çalıştırılan ring-3 smoke testinde kullanıcı entry adresi `0x400000`, ayrı kullanıcı stack’i, `SYSCALL` girişi ve syscall sonrası scheduler handoff doğrulandı. `sys_exit`, süreci zombie durumuna geçirip `scheduler_yield()` çağırıyor; runnable hedef yoksa tekrar kullanıcı frame’ine dönmek yerine ring-0 idle/halt yoluna geçiyor. Üretim build’inde demo tekrar `0` yapıldı.

## Preemptive scheduler, fork/COW ve wait4

`fork.elf` geçici demo olarak etkinleştirilerek aşağıdaki zincir gözlendi:

1. Parent process `fork` syscall’ı ile child PID oluşturdu.
2. Child address-space’i low-half page-table dallarını COW olarak klonladı.
3. Child’ın writable shared page’e yazması `0x401000` civarında write page fault üretti; fault COW çözümleyici tarafından ayrıştırıldı.
4. Child `exit(0)` ile zombie oldu.
5. Parent `wait4` syscall’ı için `THREAD_BLOCKED` durumuna geçti.
6. Child exit parent waiter’ını uyandırdı; exit status kullanıcı belleğine kopyalandı ve child PID döndürüldü.
7. Reaper child VMA listesini, address-space root’unu, FD sahipliğini ve kernel stack’ini serbest bıraktı.
8. Parent aynı syscall frame’i üzerinden kullanıcıya döndü ve kendi exit yolunu tamamladı.

İlk denemede parent’ın blocked syscall frame’inin scheduler tarafından silinmesi ve child reaper sonrasında eski thread’in privilege bilgisinin okunması iki ayrı hata olarak yakalandı. Scheduler artık blocked thread’in raw syscall frame’ini koruyor ve reaper thread metadata’sını sıfırlamadan önce kaynak CPL bilgisini saklıyor. Bu düzeltmelerden sonra fork/wait4 smoke kaydı iki process exit ve panic olmadan tamamlandı.

## mmap, lazy allocation ve stack davranışı

`mmap` syscall’ı Linux x86_64 register düzenindeki `addr, length, prot, flags, fd, offset` değerlerini anonymous veya bounded file-backed VM sözleşmesine bağlar. Anonymous mapping ilk aşamada yalnızca VMA rezervasyonu yapar; fiziksel sayfa ilk erişimde page-fault handler tarafından materialize edilir. Immutable initramfs regular file mapping’leri gerçek file offset’inden lazy doldurulur, EOF sonrası alan zero-fill edilir ve writable `MAP_SHARED` ile pseudo/persistent kaynaklar reddedilir. `munmap` VMA ve mapping temizliğini, `brk` ise process heap sınırını kullanır. Kullanıcı stack’i için izin verilen büyüme aralığı ve user-range kontrolleri page-fault dispatcher’a bağlıdır. Kanıt: [`docs/file-backed-mmap-smoke.log`](file-backed-mmap-smoke.log).

## Kullanıcı fault isolation testleri

İki geçici payload ile fault yolları ayrıca doğrulandı; test sonrasında payload’lar production kaynaklarına bırakılmadı.

| Test | Tetikleyici | Beklenen davranış | Sonuç |
|---|---|---|---|
| User #GP | Ring 3’ten `out dx, al` | Süreci 139 ile sonlandır, kernel panic yapma | Başarılı |
| User #PF | VMA dışındaki `0x200000` adresine yazma | Lazy/COW/stack koşulu yoksa süreci 139 ile sonlandır | Başarılı |

Her iki testte serial log `process exited`, ilgili fault termination kaydı ve scheduler handoff içerdi; `fatal page fault` veya kernel panic oluşmadı. Exception stub’ları CPU’nun exception sırasında otomatik `SWAPGS` yapmadığını dikkate alır. Kullanıcıdan gelen page fault/general-protection fault için kernel GS alanına geçer, handler dönüşünde gerekirse kullanıcı GS alanını geri yükler.

## TTY, display ve yapılandırılmış input

GOP yalnızca text-cell terminal surface olarak kullanılır. Limine 1920×1080×32 modu ister; firmware bu modu sağlayamazsa mevcut GOP modu kullanılır. Font 10×16 bold bitmap glyph’lerden oluşur ve hücreler 2× ölçeklenir. Console newline, tab, backspace, satır kaydırma ve Türkçe karakterlerin UTF-8 çıktı yolunu destekler. GUI, pencere, panel veya sprite çizimi yoktur. PS/2 set-1 keyboard path Türkçe Q düzenini ve `ş ğ ü ö ç İ Ş Ğ Ü Ö Ç` karakterlerini tanır.

Yeni display/input smoke’unda geçici ring-3 payload’ı `NPK_SYS_NPK_DISPLAY_INFO`, `DISPLAY_CLAIM`, owner-only `INPUT_READ`, kernel-controlled `DISPLAY_MAP`, `DISPLAY_RELEASE` ve `exit(0)` sırasını tamamladı; mapped framebuffer byte’ına erişim page fault üretmedi. Display map fiziksel adres/uzunluk kabul etmedi ve release sırasında device VMA revoke edildi. Kanıt: [`docs/display-input-smoke.log`](display-input-smoke.log).

PS/2 mouse backend’i controller auxiliary portunu etkinleştirip IRQ12’de üç-byte paketleri REL X/Y ve üç temel button event’ine çevirir. PCI + mouse production QEMU smoke’unda `pci: enumerated devices`, `mouse: PS/2 three-byte pointer events enabled` ve keyboard initialization görüldü; panic/fault yoktu. Kanıt: [`docs/pci-mouse-smoke.log`](pci-mouse-smoke.log).

## ACPI ve güç yönetimi

BIOS ve UEFI yollarında Limine RSDP üzerinden RSDT/XSDT, FADT, DSDT ve MADT bounded signature/checksum kontrolleriyle taranır. FADT PM1 bloklarını, DSDT `_S5_` sleep type değerlerini ve MADT SCI IOAPIC route’unu sağlar. Power-button SCI yolu ve PM1 S5 soft-off backend’i `docs/acpi-verification.md` içinde ayrıca açıklanmıştır.

Ham seri kayıtları [`docs/file-backed-mmap-smoke.log`](file-backed-mmap-smoke.log), [`docs/shared-memory-ipc-smoke.log`](shared-memory-ipc-smoke.log), [`docs/display-input-smoke.log`](display-input-smoke.log), [`docs/vfs-directory-smoke.log`](vfs-directory-smoke.log), [`docs/pci-enumeration-smoke.log`](pci-enumeration-smoke.log), [`docs/pci-mouse-smoke.log`](pci-mouse-smoke.log), `docs/verification-bios.log`, `docs/verification-uefi.log`, `docs/verification-fork-wait4.log`, `docs/verification-user-gp.log`, `docs/verification-user-pf.log`, `docs/verification-abi-tls.log`, `docs/verification-abi-iov.log`, `docs/verification-abi-sigaction.log`, `docs/qemu-abi-smoke-2.log`, `docs/qemu-signal-mask-smoke-2.log`, `docs/clone-futex-production-smoke.log`, `docs/futex-thread-smoke.log`, `docs/realtime-signal-smoke.log`, `docs/signal-queue-overflow-smoke.log`, `docs/dynamic-handoff-smoke.log`, `docs/poll-timeout-smoke.log`, `docs/poll-epoll-smoke.log`, `docs/smp-percpu-smoke.log`, `docs/final-production-bios.log`, `docs/final-production-uefi.log`, `docs/persistent-production-pc.log`, `docs/elf-adversarial-expanded-run.log`, `docs/final-release-bios.log`, `docs/final-release-uefi.log` ve `docs/final-release-persistent.log` dosyalarında tutulur. ABI dispatcher derlemesi, 38-vaka corpus kapısı ve BIOS/UEFI/SMP production smoke testleri başarılıdır.

## ABI runtime smoke kapsamı

`arch_prctl` syscall’ı artık Linux x86_64 numarası 158 üzerinden `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` ve `ARCH_GET_GS` işlemlerini destekler. FS base per-thread olarak `IA32_FS_BASE` MSR’ına yüklenir; kullanıcı GS base ise `IA32_KERNEL_GS_BASE` alanında saklanır ve scheduler aktivasyonunda geri yüklenir. Bu ayrım, `SWAPGS` kullanan syscall/IRQ/exception girişlerinde kernel GS alanının kullanıcı TLS değerleriyle bozulmasını önler. BIOS smoke payload’ı dört `arch_prctl` işlemi ve `sched_yield` çağrısından sonra `exit(0)` ile tamamlandı; panic veya page fault oluşmadı.

`sched_yield` mevcut scheduler yield yoluna bağlanmıştır. `readv` ve `writev`, iovec dizisini önce user-range ve `copyin` kontrollerinden geçirir; her elemanın buffer aktarımı mevcut `read`/`write` ve VFS sınırları içinde yapılır. `rt_sigaction` process başına action kaydı oluşturur ve `fork` sırasında kopyalar. Signal action kaydı artık yalnızca cosmetic değildir: kullanıcı fault’u veya `kill`/`tgkill` sonrası kayıtlı handler’a geçiş, blocked/pending bitset’i, action maskesi, minimal user signal frame kurulumu ve `rt_sigreturn` ile özgün frame’in geri yüklenmesi uygulanmıştır.

Üretim kapsamındaki yeni yollar bounded’dır: dynamic ELF için kernel `PT_INTERP` interpreter handoff ve auxv hazırlığı yapar, fakat `DT_NEEDED`/sembol çözümleme/relocation userspace `ld.so` sorumluluğundadır. Thread-group yolu TID/TGID, `clone`, `set_tid_address`, clear-child-tid ve `exit_group` içerir; robust-list, PI/requeue/bitset futexleri, cancellation/join ayrıntıları ve tam pthread/Rust runtime semantiği yoktur. Limine MP/AP discovery ve QEMU `-smp 2` bring-up vardır, ancak per-CPU scheduler, IPI/timer routing ve migration etkin değildir. `poll`/`ppoll` ve level-triggered `epoll` regular file, pipe ve TTY readiness’i için uygulanmıştır; blocking wait queue, `select` ve edge-triggered epoll yoktur. Persistent NPKFS `/persist` için fixed-region ATA PIO metadata/data writeback, sync ve readback vardır; bounded dual-slot metadata recovery eklidir, fakat data journaling/data checksum, gerçek crash consistency ve persistent directory tree yoktur. Initramfs tarafında bounded directory hierarchy ve canonical path normalization ayrıca doğrulanmıştır. Realtime signal queue `siginfo_t`/`si_value` payload’ını `SA_SIGINFO` frame’ine taşır; sigwait, nested frame ve tam Linux signal kuyruğu semantiği ayrıca açıktır. Tanımsız syscall’lar sessizce başarılı sayılmaz; `-ENOSYS` döndürür. Gerçek musl/glibc runtime testi ve geniş fault-injection hâlâ ayrı doğrulama kapılarıdır.

## Audit raporundaki beş kernel-side düzeltme

Aşağıdaki beş sorun kaynak düzeyinde giderildi ve temiz üretim derlemesinden önce geçici smoke-test kodu kaldırıldı. Exception ve signal davranışı, x86_64 giriş/çıkış frame düzeni ve Linux ABI sözleşmeleriyle uyumlu olacak şekilde doğrulandı [1] [2].

| Sorun | Düzeltme | Doğrulama sonucu |
|---|---|---|
| Ring-3 CPU exception’larının kernel’i panic etmesi | Tüm exception stubları GPR ve privilege-aware frame oluşturuyor; user-mode exception’lar ilgili Linux sinyaline çevrilip handler yoksa yalnızca ilgili process sonlandırılıyor. | `UD2` ile yapılan kullanıcı testi process exit ve scheduler handoff ile tamamlandı; kernel çalışmaya devam etti, panic oluşmadı. |
| Signal delivery’nin yalnızca kayıt tutması | `rt_sigaction` action kayıtlarına ek olarak handler yönlendirmesi, user stack signal frame’i, frame doğrulaması ve `rt_sigreturn` restore yolu eklendi. | SIGILL handler + `UD2` testinde signal handler’a geçildi; handler `exit(0)` çağırdı ve process temiz biçimde çıktı. |
| `execve` sonrasında erişilebilir dead code | Başarılı `process_launch_user()` dönüşünden sonraki hatalı `-EFAULT` dönüşü `__builtin_unreachable()` ile kaldırıldı. | Kaynak incelemesi ve temiz Clang derlemesi başarılı; başarılı exec yolunda sahte hata dönüşü kalmadı. |
| `sys_read` için 512-byte üst sınır | Dosya/VFS okumaları 4096-byte parçalarla döngü halinde `count` kadar sürdürülüyor; terminal davranışı ayrı tutuluyor. | Kaynak incelemesi ve temiz derleme başarılı; tek çağrıdaki sabit 512-byte truncation kaldırıldı. |
| `getcwd` adres döndürmesi | Başarı dönüşü user buffer adresi yerine kopyalanan yolun byte sayısını döndürüyor; yetersiz buffer `-ERANGE` veriyor. | Linux x86_64 syscall dönüş ABI’siyle kaynak incelemesi ve temiz derleme doğrulandı [1]. |

## Audit düzeltmeleri sonrası temiz BIOS/UEFI doğrulaması

Üretim ayarları (`NPK_ENABLE_USER_DEMO=0`) ile `make clean && make -j2 all disk` başarılı oldu. BIOS/SeaBIOS ve UEFI/OVMF yollarında QEMU, 512 MiB RAM ve `-no-reboot -no-shutdown` seçenekleriyle çalıştırıldı. Her iki süreç de kernel idle loop’u nedeniyle beklenen timeout ile sonlandı; `docs/final-production-bios.log` ve `docs/final-production-uefi.log` içinde `NPKernel initialization complete; interrupts enabled` görüldü. Bu production boot kayıtlarında panic, page fault veya general-protection fault yoktur ve `user: launching ring3` satırı bulunmaz. Production `user/hello.asm` yalnızca `exit(0)` içerir; signal, clone ve futex payload’ları kaynak ağacına bırakılmamıştır.

## Clone/futex, execve, brk ve wait4 kernel smoke kanıtı

Geçici clone/futex payload’ı production payload’ı tekrar minimal `exit(0)` programına döndürülmeden önce BIOS QEMU’da çalıştırıldı; ham kayıt `docs/clone-futex-production-smoke.log` dosyasındadır. Payload iki lazy anonymous mapping oluşturdu, `CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD` ile child TID `3` üretti ve parent’ı futex üzerinde bekletti. Child shared word’ü güncelledikten sonra `FUTEX_WAKE` çağırdı ve `exit` ile yalnızca kendi thread’ini sonlandırdı; parent `FUTEX_WAIT` dönüşünden sonra tek byte yazıp process exit yaptı.

| Kernel yolu | QEMU/source kanıtı | Sonuç |
|---|---|---|
| `clone` (56) | Logda syscall `56`, `clone flags = 0x10f00` ve `clone child tid = 3` | Desteklenen thread subset’i child thread oluşturdu |
| `FUTEX_WAIT`/`FUTEX_WAKE` (202) | Logda iki adet syscall `202` (`0xca`) görülür; biri parent wait, biri child wake akışıdır | Waiter uyandı; signal/timeout olmayan normal yol tamamlandı |
| Thread exit | Child sonrasında `thread exit syscall completed; yielding to scheduler` görülür; process hemen kapanmaz | `SYS_exit` clone child için thread-only exit yoluna bağlı |
| `execve` (59) | `process_exec_current()` source path’i aynı PID/TID üzerinde stage/commit, `FD_CLOEXEC` close, signal/TLS reset ve eski root destroy adımlarını içerir | In-place replacement regresyon kapısı korunur |
| `brk` | `vm_resize_heap()` heap VMA rezervasyonunu resize öncesi kontrol eder; başarısız genişleme rollback ile eski sınırda kalır; heap VMA user `munmap`/`mprotect` ile parçalanamaz | VMA reservation/rollback düzeltmesi korunur |
| `wait4` | `wake_parent_waiter()` parent wait state’e status/result ve child claim yazar; resume sonrası `process_wait4()` claimed zombie’yi reap eder | Wake/reap yarışındaki çift tüketim kapatılmıştır |

Clone/futex kaydındaki ilk user page fault, lazy anonymous mapping’in ilk yazma erişiminde materialization için alınan beklenen not-present fault’tur; handler sonrasında akış devam eder. Kayıtta kernel panic veya beklenmeyen fatal exception yoktur.

## Yeni signal, VM ve fd ABI smoke testi

Geçici ring-3 ABI payload’ı BIOS QEMU’da çalıştırıldı ve test sonunda üretim payload’ı tekrar minimal `exit(0)` programına döndürüldü. Ham kayıt `docs/qemu-abi-smoke-2.log` dosyasındadır. Test programı aşağıdaki zinciri tek process içinde yürüttü:

| Aşama | Beklenen sonuç | QEMU kanıtı |
|---|---|---|
| `rt_sigaction(SIGUSR1)` | Action ve restorer kaydı başarılı | Syscall `13` döndü; sonraki kullanıcı syscall’ına geçildi |
| `mmap` ve `mprotect(PROT_READ\|PROT_EXEC)` | Lazy VMA izinleri değişti | Syscall `9`, ardından syscall `10`; mprotect sonucu `0` |
| `mprotect(PROT_WRITE\|PROT_EXEC)` | W^X nedeniyle reddedildi | Aynı adres için mprotect sonucu `0x00000000ffffffea` (`-EINVAL`) |
| `pipe`, `write`, `dup2`, `close`, `read` | Pipe ring buffer ve logical fd paylaşımı çalıştı | Syscall sırası `22`, `1`, `33`, `3`, `1`, `0` tamamlandı |
| `fcntl(F_DUPFD)` ve `close` | VFS open-handle refcount’u korundu | Syscall `72` ve takip eden close başarıyla döndü |
| `rt_sigprocmask` + `kill(pid,SIGUSR1)` + unblock | Blocked signal pending bitset’te tutuldu; unblock sonrasında handler çağrıldı | `docs/qemu-signal-mask-smoke-2.log`: syscall `13`, `14`, `39`, `62`, ikinci `14`, ardından handler’ın `rt_sigreturn` syscall `15` ve `exit(0)` syscall `60` görüldü |
| `getpid`, `kill(pid,SIGUSR1)`, `rt_sigreturn` | Handler çağrıldı, özgün frame/mask geri yüklendi | Syscall `39`, `62`, `15` tamamlandı; ardından handler’ın `exit(0)` yolu olan syscall `60` görüldü |

Seri kayıtta `PANIC`, `#PF`, general-protection fault veya beklenmeyen termination bulunmadı. Program, `handler_ran` bayrağını yalnızca signal handler içinde set ettiği için son `exit(0)` dönüşü signal delivery ve sigreturn zincirinin uçtan uca geçtiğini doğrular.

## Yenilenmiş arşiv bağımsız build doğrulaması

Yüklenen `NPKernel-v0.3-userspace(7).tar.gz` arşivi güvenli biçimde açılıp mevcut çalışma ağacıyla karşılaştırıldı. Kaynak, belge ve helper dosyalarının tamamı önceki production ağacıyla aynıydı; arşivde generated `build/` dizini bulunmuyordu. Bu karşılaştırma sırasında initramfs helper’ında `/home/ubuntu/npkernel` konumuna sabitlenmiş iki yol tespit edildi. Başka bir dizine çıkarılmış arşiv bu nedenle `build/initramfs.cpio` dosyasını kendi build dizinine yazmıyor ve ISO hedefi başarısız oluyordu.

Düzeltme olarak `tools/make_initramfs.py` proje kökünü `__file__` üzerinden keşfedecek ve çıktı yolunu komut satırı argümanı kabul edecek şekilde güncellendi. Makefile artık `python3 tools/make_initramfs.py $(INITRAMFS)` çağrısını kullanıyor; geriye dönük olarak argümansız çağrı da proje kökü altındaki `build/initramfs.cpio` varsayılanını koruyor.

Düzeltilmiş arşiv kopyası `/home/ubuntu/incoming/npkernel-renewed/npkernel` altında bağımsız olarak temiz derlendi. `build/npkernel.elf`, `build/npkernel.iso`, `build/npkernel-disk.img` ve `build/initramfs.cpio` üretildi; Limine BIOS kurulumu başarıyla tamamlandı. Böylece release ağacının checkout/çıkarma konumundan bağımsız build edilebildiği doğrulandı.

| Kontrol | Sonuç |
|---|---|
| Gzip ve tar bütünlüğü | Başarılı; şüpheli mutlak veya `..` yolu yok |
| Production guard ve minimal hello | Başarılı; `NPK_ENABLE_USER_DEMO=0`, payload `exit(0)` |
| Extracted archive independent build | Başarılı; initramfs, ISO ve disk image üretildi |
| Kernel ELF | ELF64 `EXEC`, x86_64 |
| Existing ABI/security documentation | Güncel; dynamic ELF handoff, futex/clone bounded subset ve SMP/persistent/realtime sınırları açıkça belgeleniyor |

## Yeni kernel feature doğrulaması

### Dynamic ELF handoff

`elf64_validate()` artık `PT_INTERP` ve `PT_DYNAMIC` için bounded metadata kontrolleri uygular. `stage_exec_image()` doğrulanmış interpreter yolunu VFS’den okur, ikinci image’ı güvenli ve çakışmayan ET_DYN bias’ında aynı user address-space’e yükler ve `AT_PHDR`, `AT_PHNUM`, `AT_PHENT`, `AT_BASE` ve ana image’ın `AT_ENTRY` değerlerini initial stack’e koyar. Bu doğrulama `docs/dynamic-handoff-smoke.log` içindeki sentetik interpreter image’ı ile handoff seviyesindedir; initramfs’e gerçek glibc/musl `ld.so` veya relocation yapan userspace runtime eklenmiş değildir. Kullanıcı interpreter’ı (`ld.so`) `DT_NEEDED` grafını, sembolleri, TLS image’ını ve relocation’ları çözmekten sorumludur; kernel bu işlemleri userspace yerine geçecek şekilde üstlenmez. Nested interpreter yolu reddedilir ve stage başarısızlığında yeni root/VMA’lar rollback edilir.

### Thread-group ve realtime signal payload

Thread-group yolu TID/TGID, bounded `clone`, `set_tid_address`, `clear_child_tid`, `exit_group` ve group-wide exit/signal alanlarını taşır. Temel pthread join/wait paterni için clear-child-tid futex wake yolu vardır; robust-list, PI/requeue/bitset futexleri, cancellation ve tüm pthread/Rust runtime semantiği kapsam dışıdır.

Realtime signal yolu `rt_sigqueueinfo` ve `rt_tgsigqueueinfo` üzerinden kullanıcı `siginfo_t` verisini bounded copyin ile alır. Yalnızca `SIGRTMIN..SIGRTMAX` queue’ya alınır; `si_code`, `si_pid`, `si_uid` ve `si_value` alanları queue entry’ye taşınır. Blocked delivery düşük signal numarası önceliğiyle, aynı signal için FIFO kuyrukta bekler; `SA_SIGINFO` frame’i payload’ı kullanıcı handler’ına verir ve kuyruk dolduğunda gönderici `-EAGAIN` alır. `rt_sigtimedwait`, sigwait ailesi ve çoklu nested frame semantiği henüz yoktur.

### poll/epoll ve persistent storage

`poll`/`ppoll` regular file, pipe ve TTY readiness’ini bounded descriptor taramasıyla döndürür. `epoll` bounded watch tablosu ile level-triggered çalışır; `EPOLL_CTL_ADD/MOD/DEL`, `EPOLLIN`, `EPOLLOUT`, `EPOLLERR` ve close sırasında refcount-safe watch temizliği uygulanır. Timeout PIT tick’leri üzerinden sınırlandırılır; blocking pipe wait queue, `select` ve edge-triggered epoll kapsam dışıdır.

ATA PIO write yolu ile fixed-region NPKFS `/persist` backend’i vardır. Superblock/inode metadata bounded validation’dan geçer; çift metadata slotu, checksum, generation ve transaction marker ile yarım metadata commit’leri için sınırlı metadata recovery uygulanır. `O_CREAT`, `O_TRUNC`, `O_APPEND`, read/write, close flush, `fsync`, `fdatasync` ve `sync` yolları metadata/data region’a yazar. Bu dosya sistemi data-block checksum’ı, data journaling, gerçek güç-kesintisi crash consistency, persistent directory tree ve fault-injection garantisi vermez. Immutable initramfs VFS ise bounded directory hierarchy ve canonical absolute path smoke’undan geçti; kanıt: [`docs/vfs-directory-smoke.log`](vfs-directory-smoke.log).

### Shared-memory IPC, VFS hierarchy ve PCI

Shared-memory smoke payload’ı bounded anonymous object oluşturdu, parent/child arasında descriptor transfer etti, iki address-space’te `MAP_SHARED` aynı fiziksel byte görünürlüğünü doğruladı ve her iki process exit status’ını 0 ile tamamladı. Kanıt: [`docs/shared-memory-ipc-smoke.log`](shared-memory-ipc-smoke.log).

Initramfs path smoke’u `/bin/./hello.elf` canonicalization, root ve `/bin` `getdents64`, bounded child directory discovery ve `/bin/../hello.elf` traversal reddini doğruladı. Bu, persistent filesystem veya tam POSIX path namespace testi değildir. PCI smoke’u config mechanism #1 bounded enumeration’ın q35 üzerinde boot’u bozmadığını gösterdi.

### SMP bring-up ve ELF corpus

Limine MP response boot request’e bağlıdır. QEMU `-smp 2` testinde AP keşfi ve AP online callback’i görüldü; AP’ler güvenli idle loop’ta tutulduğu için bu sonuç **SMP bring-up** kanıtıdır, per-CPU scheduler veya migration kanıtı değildir. Scheduler hâlen BSP’nin PIT tabanlı run path’idir.

`make test-elf-corpus` hedefi `tools/elf_adversarial.py` ile 38 bozuk/şüpheli image üretir ve `tests/elf_validate_host.c` harness’iyle validator’a bağlar. Corpus; truncated/header/version, program-header overflow, bad alignment, load overlap, W+X, noncanonical entry/range, huge `memsz`, duplicate/unterminated/relative/traversal `PT_INTERP` ve malformed `PT_DYNAMIC` sınıflarını kapsar. Beklenen politika geçerli image dışındaki tüm vakaları reddetmektir; son çalıştırmada 38 vaka 0 hata ile sonuçlandı. Bu host validation, userspace dynamic relocation uyumluluğunu test etmez.

## Kaynak referansları

[1]: https://www.kernel.org/doc/html/latest/arch/x86/entry_64.html "Linux x86_64 entry documentation"
[2]: https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html "System V ABI ELF specification"
[3]: https://github.com/Limine-Bootloader/Limine/blob/v12.x/CONFIG.md "Limine configuration documentation"
[4]: https://uefi.org/specifications "UEFI specifications"
[5]: https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/04_ACPI_Hardware_Specification/ACPI_Hardware_Specification.html "ACPI Hardware Specification"

## v0.4 kernel-only platform release gate

Phase 2–5 kernel-only genişletmeleri sonrasında üretim kaynak ağacında `NPK_ENABLE_USER_DEMO=0` ve minimal `user/hello.asm` (`exit(0)`) korunarak `make clean && make -j2 all disk` başarıyla tamamlandı. `make check` ve `make test-elf-corpus` de başarılıdır; corpus sonucu `ELF_CORPUS total=38 failed=0` oldu.

Yeni kernel sözleşmeleri şu smoke kayıtlarıyla doğrulandı: immutable initramfs file-backed `mmap` (`docs/file-backed-mmap-smoke.log`), bounded shared-memory object ve parent/child descriptor transfer (`docs/shared-memory-ipc-smoke.log`), safe GOP display lease/input mapping (`docs/display-input-smoke.log`), initramfs directory hierarchy/canonical path (`docs/vfs-directory-smoke.log`), PCI configuration enumeration (`docs/pci-enumeration-smoke.log`) ve PS/2 mouse/PCI production initialization (`docs/pci-mouse-smoke.log`). Bu kayıtların geçici ELF payload’ları production kaynağında bırakılmadı.

Fresh production QEMU matrix:

| Yol | QEMU sonucu | Kanıt |
|---|---:|---|
| BIOS/SeaBIOS `-M pc` | Expected timeout `124`; initialization complete, PCI/mouse, no fatal marker | [`docs/final-v04-bios.log`](final-v04-bios.log) |
| Q35 `-smp 2` | Expected timeout `124`; AP discovery/online, initialization complete, no fatal marker | [`docs/final-v04-smp2.log`](final-v04-smp2.log) |
| Legacy PC + IDE persistent path | Expected timeout `124`; persistent NPKFS formatted/recovered path and initialization complete, no fatal marker | [`docs/final-v04-pc-disk.log`](final-v04-pc-disk.log) |
| UEFI/OVMF Q35 | Expected timeout `124`; initialization complete, PCI/mouse, no fatal marker | [`docs/final-v04-uefi.log`](final-v04-uefi.log) |

Q35’ye `if=ide` ile disk eklenen ayrı denemede firmware/controller modeli ATA PIO primary cihazı sunmadı ve kernel bunu `persistent NPKFS unavailable` uyarısıyla kontrollü biçimde raporladı; bu bir panic değildir. Persistent disk gate’i bu nedenle ATA sürücüsünün desteklediği legacy PC + IDE konfigürasyonunda değerlendirildi. Hiçbir production QEMU kaydında `user: launching`, `PANIC`, `fatal page fault` veya general-protection fault bulunmadı.
