# NPKernel kullanıcı alanı ABI v1

**Durum:** prototype kernel sözleşmesi. **Amaç:** dış geliştiricilerin daha sonra C veya Rust ile servis, yardımcı program, compositor ve GUI yazabilmesi. Bu belge bir init sistemi, servis programı, shell, libc, pthread kütüphanesi veya GUI sunmaz.

## 1. Tasarım sınırı

NPKernel’in mevcut hedefi Linux kullanıcı alanını olduğu gibi çalıştırmak değil, üzerinde bağımsız bir genel amaçlı kullanıcı alanı kurulabilecek güvenli ve ölçülebilir bir çekirdek sözleşmesi sunmaktır. `include/npk/user_abi.h` bu sözleşmenin C sabitlerini ve temel veri yapılarını taşır.

> **Önemli ayrım:** `no_std` Rust veya kendi `_start` giriş noktasına sahip statik C ELF’i için çekirdek temellerinin bulunması, Rust `std`, glibc veya musl uyumluluğunun tamamlandığı anlamına gelmez.

ABI sürümü `NPK_USER_ABI_VERSION == 1` değeridir. Desteklenmeyen syscall’lar `-ENOSYS`, geçersiz kullanıcı adresleri `-EFAULT`, diğer hatalar ilgili negatif errno ile döner. Başarılı syscall sonucu negatif olmayan bir değerdir; pointer döndüren `mmap` için negatif sonuç hata olarak yorumlanmalıdır.

## 2. x86_64 syscall çağrı sözleşmesi

Kullanıcı programı `syscall` komutundan önce syscall numarasını `rax` içine, en fazla altı tamsayı/pointer argümanı aşağıdaki register’lara koyar. Bu sıra Linux AMD64 kernel interface tanımıyla aynıdır; kullanıcı C fonksiyon çağrısının dördüncü argüman register’ı olan `rcx` burada `r10` olur.[1]

| Register | Anlam |
|---|---|
| `rax` | Syscall numarası; dönüşte sonuç veya `-errno` |
| `rdi` | 1. argüman |
| `rsi` | 2. argüman |
| `rdx` | 3. argüman |
| `r10` | 4. argüman |
| `r8` | 5. argüman |
| `r9` | 6. argüman |
| `rcx`, `r11` | CPU `syscall` geçişi tarafından clobber edilir |

Çekirdek, syscall numaralarını `include/npk/syscall.h` ve dış geliştiricilerin kullanacağı eşdeğer sabitleri `include/npk/user_abi.h` içinde tutar. Bu iki başlık uygulama kütüphanesi değildir; pointer doğrulama, errno çevirisi, yeniden deneme ve thread-local errno kullanıcı runtime’ının sorumluluğundadır.

## 3. ELF yükleme ve process başlangıcı

Kernel, güvenlik kontrollerinden geçmiş `ET_EXEC` ve sınırlı `ET_DYN` ELF görüntülerini kullanıcı adres alanına yükler. `ET_DYN` ana görüntü için sabit ve çakışma denetimli bir yükleme tabanı kullanılır. PT_LOAD aralıkları, `p_filesz <= p_memsz`, overflow, hizalama, canonical kullanıcı aralığı, segment çakışması, W^X ve page permission koşulları denetlenir.

Tek seviyeli bounded interpreter handoff desteği vardır. Ana ELF güvenli bir PT_INTERP yolu içeriyorsa interpreter VFS’ten okunur, `ET_DYN` olarak ayrı tabana yüklenir ve başlangıç RIP interpreter entry point’ine verilir. Interpreter içinde ikinci PT_INTERP reddedilir. Kernel, interpreter’ın relocation işlemlerini yapmaz; tam `ld.so`, GOT/PLT relocation, shared-object dependency resolution, symbol versioning ve TLS module loader davranışı bu sürümün parçası değildir. ELF’in program interpreter ve dynamic-linking kavramları GABI’de ayrıca tanımlanır.[4]

Yeni kullanıcı stack’i şu sırayı izler:

```text
argc
argv[0..argc-1]
NULL
envp[0..envc-1]
NULL
auxv[0].type, auxv[0].value
...
AT_NULL, 0
```

Mevcut auxv kayıtları aşağıdaki tabloda verilmiştir. `AT_BASE` yalnızca interpreter handoff gerçekleştiğinde eklenir; `AT_ENTRY` ana ELF entry point’ini, `AT_PHDR` ise ana ELF’in program-header adresini gösterir.

| Auxv tipi | Değer |
|---|---|
| `AT_PHDR` | Ana ELF program-header sanal adresi |
| `AT_PHENT` | Program-header entry boyutu |
| `AT_PHNUM` | Program-header sayısı |
| `AT_PAGESZ` | `NPK_PAGE_SIZE` |
| `AT_BASE` | Interpreter load bias; yalnızca interpreter varsa |
| `AT_ENTRY` | Ana ELF entry point’i |
| `AT_NULL` | Auxv sonu |

Kernel şu anda `AT_RANDOM`, `AT_EXECFN`, `AT_SYSINFO_EHDR`, `AT_HWCAP`, `AT_PLATFORM` ve vDSO sözleşmesi sağlamaz. Auxv, işletim sistemi tarafından process başlangıcında çekirdek-parametrelerini kullanıcı alanına taşımak için kullanılır; kullanıcı runtime’ı yalnızca belgelenmiş kayıtları tüketmelidir.[2]

`execve` yolları şu anda sınırlı absolute-path VFS çözümlemesi kullanır. Relative cwd/name-resolution, symlink ağacı, `readlink` için genel symlink nesneleri ve tam permission namespace henüz yoktur. `/proc/self/exe` için process’in son `execve` image path’i bounded biçimde raporlanabilir.

## 4. TLS ve thread başlangıcı

`arch_prctl` aşağıdaki işlemleri destekler:

| Komut | Destek |
|---|---|
| `ARCH_SET_FS` | Mevcut thread’in FS base’ini ayarlar |
| `ARCH_GET_FS` | FS base’i kullanıcı adresine yazar |
| `ARCH_SET_GS` | User GS base’i ayarlar |
| `ARCH_GET_GS` | User GS base’i kullanıcı adresine yazar |

FS/GS değerleri thread bağlamında saklanır, context switch ve fork sırasında korunur. Kullanıcı programı TLS bloğunun adresini kendisi tahsis etmeli, `ARCH_SET_FS` ile kurmalı ve thread clone sırasında `CLONE_SETTLS` kullanmalıdır. Kernel, TLS ABI’sinin kendisini, TLS module ID’lerini veya dynamic linker TLS relocation’larını üretmez.

`clone` ile thread oluşturmanın kabul edilen zorunlu flag kümesi `CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD`’dir. `CLONE_SETTLS`, parent/child TID yazma-temizleme ve `CLONE_SYSVSEM` bounded opsiyonlardır. Child stack 16-byte hizalı kullanıcı adresi olmalıdır. Kernel `clone3`, robust futex listeleri, priority-inheritance futex, requeue/bitset işlemleri, namespaces, signal stack ve tam pthread cleanup sözleşmesini sağlamaz.

Futex v1 subseti `FUTEX_WAIT`, `FUTEX_WAKE` ve `FUTEX_PRIVATE_FLAG` temellerini içerir. Wait adresi kullanıcı alanında doğrulanır, beklenen değer eşleşmezse `-EAGAIN`, timeout gerçekleşirse `-ETIMEDOUT`, signal ile kesilirse `-EINTR` dönebilir. Thread runtime’ı bu sınırlamaları bilmeden tam POSIX pthread semantiği varsaymamalıdır.

## 5. Bellek ve mmap

Anonymous `mmap`, `munmap`, `mprotect` ve `brk` vardır. Anonymous VMA’lar lazy reservation’dır; ilk erişimde sayfa fault handler tarafından materialize edilir. Aşağıdaki temel korumalar geçerlidir:

| Özellik | Kernel v1 davranışı |
|---|---|
| `PROT_READ`, `PROT_WRITE`, `PROT_EXEC` | User page permission’a çevrilir |
| `PROT_WRITE | PROT_EXEC` | Reddedilir; W^X korunur |
| Anonymous private mapping | Lazy zero page materialization ve fork/COW |
| File-backed mapping | Yalnızca immutable initramfs regular file ve `MAP_PRIVATE` |
| `MAP_SHARED | PROT_WRITE` file mapping | Reddedilir; writeback/page cache yok |
| Dosya sonunu aşan sayfa | Sayfa sıfırla doldurulur |
| `mprotect` | VMA split ve mevcut leaf permission güncellemesi |
| `munmap` | Tam/baş/son/orta VMA bölgesi; backing handle refcount ile |

File-backed mapping’de descriptor VMA oluşturulurken bir backing handle referansı alır. VMA split, fork, partial unmap ve process destruction yolları bu sahipliği ayrı ayrı korur. İlk backend özellikle immutable initramfs regular dosyalarıdır. Procfs snapshot, pipe, epoll, directory ve persistent mutable descriptor mapping kaynağı olarak reddedilir. Persistent NPKFS metadata recovery vardır; page cache/writeback ve file data journaling bulunmadığı için bu descriptorların doğrudan map edilmesi bilinçli olarak kapalıdır.

`MAP_FIXED` prototype düzeyinde yalnızca çakışma denetimli sabit adres isteğidir; mevcut mapping’i Linux’taki replace semantiğiyle otomatik silmez. Runtime’lar güvenli varsayılan olarak `requested == NULL` kullanmalıdır.

## 6. Process, signal ve temel I/O

Mevcut process temelinde `fork`, `execve`, `wait4`, `waitpid`, `exit`, `exit_group`, `getpid`, `gettid`, `set_tid_address`, `sched_yield`, `nanosleep` ve `clock_gettime` bulunur. Process exit sırasında address space/VMA, açık descriptor ve zombie/wait durumu temizlenir.

Pipe, `dup`, `dup2`, `fcntl`, `poll`, `ppoll`, level-triggered `epoll`, `readv` ve `writev` vardır. Bunlar servisler ve ileride bir compositor etrafında küçük koordinasyon katmanı kurmak için başlangıç sağlar; ancak kernel v1’de Unix domain socket, eventfd, signalfd, socket namespace, network stack ve genel device namespace yoktur. Bunun yerine compositor/service prototiplerinin ilk shared-memory yolu için üç NPKernel extension syscall’ı vardır:

| Syscall | Register sözleşmesi | Davranış |
|---|---|---|
| `NPK_SYS_NPK_SHM_CREATE` (`0x400`) | `rdi=size` | Sıfırlanmış, anonim, en fazla 256 sayfalık shared object fd’si döndürür |
| `NPK_SYS_NPK_FD_SEND` (`0x401`) | `rdi=target_pid`, `rsi=source_fd` | Parent/child ilişkisi içindeki canlı process’e descriptor referansı kuyruğa bırakır |
| `NPK_SYS_NPK_FD_RECV` (`0x402`) | Argüman yok | Hedef process’in bounded transfer kuyruğundan yeni fd alır |

Shared object `mmap` ile yalnızca `MAP_SHARED` olarak eşlenir. `PROT_READ` ve `PROT_WRITE` desteklenir; writable+executable mapping yine reddedilir. Object’in fiziksel sayfaları sıfırlanır, mapping sayfa fault’unda aynı fiziksel sayfaları paylaşır ve fork sırasında COW’a çevrilmez. Object descriptor veya mapping kalmadığında fiziksel sayfalar PMM refcount ile serbest kalır. Object boyutu 256 sayfayla sınırlıdır; mapping object sınırını aşamaz.

Descriptor transfer queue process başına sekiz pending referansla sınırlıdır. Transfer `vfs_retain` ile yeni bir descriptor referansı tutar; receiver `FD_RECV` ile fd tablosuna kuramazsa referans kernel tarafından bırakılır. Process exit pending transferleri de kapatır. Yetki modeli prototype aşamasında sender’ın kendisi, parent’ı veya child’ı ile sınırlıdır; bu genel capability sistemi veya Unix socket ancillary-data sözleşmesi değildir.

Sinyal altyapısı `rt_sigaction`, `rt_sigprocmask`, `kill`, `tgkill`, realtime queued payload ve `rt_sigreturn` subsetini içerir. Realtime queue bounded’dir; queue dolarsa `-EAGAIN` beklenir. Tam sigaltstack, core dump, ptrace, seccomp, robust signal frame extensions ve bütün Linux signal flags sağlanmaz.

## 7. TTY ve erken display/input sözleşmesi

FD `0`, `1`, `2` kernel’in başlangıç terminaline bağlıdır. `read` FD 0 üzerinden mevcut PS/2 keyboard text queue’sundan satır parçası okuyabilir; bu henüz key-down/key-up event device değildir. `write` FD 1/2 üzerinden kernel console’a yazar.

Framebuffer doğrudan kullanıcı tarafından verilen fiziksel adresle map edilemez. Display extension sırası şöyledir:

| Syscall | Register sözleşmesi | Davranış |
|---|---|---|
| `NPK_SYS_NPK_DISPLAY_INFO` (`0x403`) | `rdi=user_info` | Limine GOP metadata’sını ve page-aligned mapping boyutunu kopyalar |
| `NPK_SYS_NPK_DISPLAY_CLAIM` (`0x404`) | Argüman yok | Canlı process için tek display lease’i alır |
| `NPK_SYS_NPK_DISPLAY_MAP` (`0x405`) | `rdi=hint`, `rsi=flags` | Yalnızca lease sahibi process’e kernel-selected fiziksel framebuffer aralığını `MAP_SHARED` read/write olarak map eder |
| `NPK_SYS_NPK_DISPLAY_RELEASE` (`0x406`) | Argüman yok | Device VMA’larını revoke eder ve lease’i bırakır |
| `NPK_SYS_NPK_INPUT_READ` (`0x407`) | `rdi=buffer`, `rsi=capacity_bytes` | Yalnızca display lease sahibine structured event kayıtlarını verir |

`NPK_USER_DISPLAY_INFO` içinde fiziksel taban, ilk sayfa içi byte offset’i, toplam mapping boyutu, width/height, pitch, bpp ve RGB mask bilgisi bulunur. `DISPLAY_MAP` fiziksel adres veya uzunluk kabul etmez; bu nedenle kullanıcı process’i arbitrary MMIO/RAM seçemez. Mapping executable değildir. Lease bırakılınca VMA’lar revoke edilir; process exit de aynı temizliği yapar. Kernel console, lease süresince GOP’a yeni hücre çizmez. Bu bir GPU acceleration veya compositor protocol’ü değil, güvenli framebuffer handoff temelidir.

Input ABI’de her kayıt 24 byte’tır: timestamp tick değeri, event type, key/scancode code, press/release value ve modifier bitleri. PS/2 set-1 keyboard şu an key press/release ve sınırlı extended arrow kodlarını üretir; eski UTF-8/text queue davranışı korunur. `REL` ve `ABS` event type’ları ABI’de mouse/touchpad backend’i için kullanılır. Kernel prototype’ında PS/2 üç-byte mouse backend’i IRQ12 üzerinden motion ve üç temel button değişimini üretir; USB HID, hotplug, çoklu cihaz routing ve genel device namespace henüz yoktur. Input queue display claim sırasında temizlenir ve process başına değil sistemde tek owner’a yönlendirilir; bu, ileride compositor’ın input router olması için bilinçli başlangıç kuralıdır.

ABI v1’de temel terminal ioctl’ları bulunur:

| Request | Davranış |
|---|---|
| `TCGETS` | Bounded terminal settings yapısını döndürür |
| `TCSETS`, `TCSETSW`, `TCSETSF` | Bounded terminal settings’i günceller |
| `TIOCGWINSZ` | GOP varsa pixel/cell ölçülerinden terminal boyutu döndürür |
| `TIOCSWINSZ` | Bounded terminal boyutunu günceller |
| `FIONREAD` | Keyboard queue boş değilse en az bir byte bulunduğunu bildirir |

Bu terminal ioctl’ları framebuffer ownership veya compositor display ABI’si değildir. Güvenli framebuffer descriptor/query ve kontrollü mapping, yapılandırılmış keyboard event, mouse/USB HID ve input routing sonraki kernel fazlarının konusudur.

## 8. C ile ilk dış runtime yolu

Kernel kaynak ağacına bir C runtime, init, servis veya yardımcı program eklenmez. Dış geliştirici kendi `_start` sembolüne sahip, `-nostdlib` ile static link edilmiş bir ELF üretebilir. Başlangıç kodu stack’ten `argc/argv/envp/auxv` okur, syscall wrapper’larını yukarıdaki register sözleşmesiyle çağırır ve negatif dönüşleri kendi errno mekanizmasına çevirir.

Minimum C toolchain varsayımı şöyledir:

```text
freestanding veya cross compiler
ELF64 x86_64 output
-nostdlib -static
kendi _start ve linker script’i
red-zone ve stack alignment kurallarına uygun assembly/C ABI
```

Compiler’ın hedefi için kernel image’ının freestanding flags’leri doğrudan kopyalanmamalıdır. Kernel `-mcmodel=kernel`, `-mno-red-zone`, SSE/FPU kullanım kısıtları ve privileged code varsayımları taşır; kullanıcı ELF’i System V AMD64 user ABI ile ayrı linklenmelidir. Kernelin mevcut static ELF yolu doğrulanmış olsa da bu, libc’nin bütün initialization syscalls’larının tamamlandığı anlamına gelmez.

## 9. Rust ile ilk dış runtime yolu

İlk Rust hedefi `#![no_std]` ve kendi `_start`/panic handler’ına sahip ELF’tir. `core` platformdan bağımsız dil primitive’lerini sağlar; `alloc` kullanılacaksa dış geliştirici kernel mmap/brk desteği üzerine kendi global allocator’ını kurmalıdır. `std`, OS abstraction ve runtime integration ister; `no_std` ortamında `std` runtime’ının process argümanlarını, main thread’i ve benzeri hosted davranışları kendiliğinden sağlamayacağı Rust dokümantasyonunda açıkça belirtilir.[3]

NPKernel şu aşamada Rust target specification, sysroot, `libstd` portu, unwinder, allocator crate, C-compatible Rust SDK veya hazır syscall crate yayımlamaz. Bu bilinçli bir kapsam sınırıdır: kernel önce ELF/TLS/memory/thread/IPC sözleşmesini stabilize eder. Dış Rust geliştiricisi için güvenli ilk katman `no_std` + inline assembly veya küçük bir harici syscall crate’idir; `std::thread`, `std::fs`, `std::net`, `std::process` ve async runtime uyumluluğu henüz hedeflenmemiştir.

## 10. Sürümleme ve uyumluluk politikası

`NPK_USER_ABI_VERSION` artmadan mevcut syscall numaraları ve header veri yapıları geriye dönük korunmaya çalışılır. Yeni syscall veya ioctl eklemek mevcut numaraları yeniden kullanmamalıdır. Davranışın genişletilmesi, özellikle şu alanlarda açık feature bit’i veya yeni ABI sürümü gerektirebilir: shared memory/writeback, display ownership, structured input, sockets, full signal frames, clone semantics ve SMP migration.

Kernel prototype olduğu için bir uygulama “syscall numarası var” diye özelliğin tam Linux semantiğine sahip olduğunu varsaymamalıdır. Runtime feature detection, kontrollü fallback ve açık `-ENOSYS`/`-ENOTTY` handling zorunludur.

## References

[1]: https://www.ucw.cz/~hubicka/papers/abi/node33.html "AMD64 Linux Kernel Conventions"
[2]: https://docs.kernel.org/arch/x86/elf_auxvec.html "Linux kernel x86 ELF auxiliary vectors"
[3]: https://docs.rust-embedded.org/book/intro/no-std.html "The Embedded Rust Book: no_std Environment"
[4]: https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html "ELF Generic ABI contents"
