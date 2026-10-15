# NPKernel syscall başvuru dokümanı

**ABI sürümü:** `NPK_USER_ABI_VERSION == 1`  
**Son kaynak kontrolü:** 2026-10-15  
**Kapsam:** Bu belge, `src/syscall/syscall.c` içindeki dispatcher’da şu anda tanımlı olan syscall yollarını ve NPKernel’e özgü extension’ları listeler. Numara ve sözleşme başlıkları `include/npk/syscall.h` ile `include/npk/user_abi.h` içinden alınmıştır.

> NPKernel, Linux x86_64 syscall numaralarının önemli bir alt kümesini kullansa da tam Linux uyumluluğu sağlamaz. Bir syscall’ın aynı numaraya sahip olması, Linux’taki tüm flag, hata, sinyal, dosya sistemi veya eşzamanlılık davranışlarının uygulandığı anlamına gelmez. Bu dokümanda belirtilen kısıtlar geçerlidir.

## 1. Çağrı sözleşmesi

Userspace, `syscall` talimatından önce syscall numarasını `RAX`’a; argümanları da aşağıdaki sırayla yerleştirir:

| Register | İçerik |
|---|---|
| `RAX` | Syscall numarası; dönüşte sonuç veya negatif errno |
| `RDI` | 1. argüman |
| `RSI` | 2. argüman |
| `RDX` | 3. argüman |
| `R10` | 4. argüman |
| `R8` | 5. argüman |
| `R9` | 6. argüman |
| `RCX`, `R11` | `syscall` geçişinde CPU tarafından değiştirilir; argüman olarak kullanılmaz |

C fonksiyon çağrısındaki dördüncü argüman register’ı `RCX` iken syscall ABI’sinde **`R10`** kullanılır. Bu çağrı sözleşmesi için en fazla altı integer/pointer argümanı aktarılır. Pointer argümanlar kullanıcı adresidir; kernel bunları doğrulayıp `copyin/copyout` yapar.

### Ham `write(1, ...)` örneği

Aşağıdaki örnek freestanding x86_64 C kodunda 1 numaralı syscall’ı, yani `write`, doğrudan çağırır:

```c
static long npk_write(int fd, const void *buffer, unsigned long count) {
    long result = 1; /* __NR_write */
    register long rdi __asm__("rdi") = fd;
    register const void *rsi __asm__("rsi") = buffer;
    register unsigned long rdx __asm__("rdx") = count;
    __asm__ volatile (
        "syscall"
        : "+a"(result)
        : "D"(rdi), "S"(rsi), "d"(rdx)
        : "rcx", "r11", "memory"
    );
    return result; /* Başarı byte sayısı; hata negatif errno */
}
```

Uygulamada her çağrı için böyle wrapper yazmak yerine uygun libc/runtime wrapper’ı kullanın. NPKernel’in `user/libc` dizinindeki mini runtime yalnızca sınırlı bir kullanıcı programı örneğidir; genel amaçlı glibc/musl değildir.

### Dönüş ve hata işleme

- Başarıda syscall, syscall’a özgü negatif olmayan sonuç döndürür: örneğin byte sayısı, fd, PID veya `0`.
- Hata doğrudan **`-errno`** biçiminde döner; ABI başlığındaki hata aralığı `-4095..-1`’dir. Kernel ham ABI’de `errno` değişkenini kendisi ayarlamaz; libc wrapper’ı gerekirse negatif sonucu `-1`’e çevirip `errno` yazar.
- Tanınmayan/uygulanmayan numara `-ENOSYS` döndürür; sessizce başarılı sayılmaz.
- `mmap` pointer benzeri bir değer döndürür; hata olup olmadığını yalnızca `NULL` ile kontrol etmeyin. Linux ABI kuralı gibi `-4095..-1` aralığını veya `NPK_USER_MAP_FAILED`’i kontrol edin.
- `exit`, `exit_group` ve başarılı `execve` normal dönüş yapmaz. `execve` başarılı olursa mevcut process’in image’ı değiştirilir.

## 2. Dosya, terminal ve metadata syscall’ları

Argümanlar aşağıdaki tabloda syscall register sırasıyla verilmiştir. `path` NUL-sonlandırılmış kullanıcı string’idir; `buf`/`out` kullanıcı belleğine işaret eder.

| Numara | Ad | Argümanlar | Kısa özet ve kapsam |
|---:|---|---|---|
| 0 | `read` | `fd, buf, count` | Dosya, pipe veya FD 0’dan byte okur. FD 0 varsayılan canonical modda satır tamamlanana dek bekler; TTY ayarıyla raw karakter okuma da vardır. Düzenli VFS okumaları istenen miktarı sınırlı parçalarla sürdürebilir. |
| 1 | `write` | `fd, buf, count` | Dosyaya/pipe’a yazar; FD 1 ve 2 kernel console’a çıkar. Dönüş yazılan byte sayısıdır. |
| 2 | `open` | `path, flags, mode` | Process’in cwd’sine göre yolu çözüp dosya/dizin descriptor’ı açar; desteklenen VFS ve NPKFS işlemleriyle sınırlıdır. `mode`, mevcut create izin/semantiğinin sınırları içindedir. |
| 3 | `close` | `fd` | Process’in descriptor slotunu kapatır ve referansını bırakır. |
| 4 | `stat` | `path, statbuf` | Yolu çözüp dosya metadata’sını Linux-benzeri `stat` düzeninde kopyalar. |
| 5 | `fstat` | `fd, statbuf` | Açık descriptor metadata’sını verir. |
| 7 | `poll` | `fds, nfds, timeout_ms` | Regular file, pipe ve TTY readiness’ini sınırlandırılmış taramayla bildirir; negatif timeout beklemeyi ifade eder, nonnegative timeout milisaniyedir. Wait-queue semantiği ve `select` yoktur. |
| 8 | `lseek` | `fd, offset, whence` | Desteklenen seekable descriptor’ın paylaşılan offset’ini değiştirir ve yeni offset’i döndürür. |
| 16 | `ioctl` | `fd, request, arg` | Şu an esas olarak terminal kontrolü içindir: `TCGETS`, `TCSETS`, `TCSETSW`, `TCSETSF`, `TIOCGWINSZ`, `TIOCSWINSZ`, `FIONREAD`. Genel Linux ioctl aygıt modeli değildir. |
| 19 | `readv` | `fd, iov, iovcnt` | Sınırlı iovec dizisini doğrulayarak birden çok buffer’a okur. |
| 20 | `writev` | `fd, iov, iovcnt` | Sınırlı iovec dizisini doğrulayarak birden çok buffer’dan yazar. |
| 22 | `pipe` | `int pipefd[2]` | Pipe’ın okuma/yazma uçlarını iki fd olarak kullanıcı dizisine yazar. Pipe buffer’ı bounded ve nonblocking’dir; boş pipe `-EAGAIN`, okuyucusu kalmayan pipe’a yazma `-EPIPE` verebilir. |
| 32 | `dup` | `oldfd` | En küçük uygun fd slotuna yeni descriptor kopyası oluşturur. |
| 33 | `dup2` | `oldfd, newfd` | Descriptor referansını belirtilen slotta kopyalar; aynı fd için davranış kernel handler’ının kurallarıyla sınırlıdır. |
| 72 | `fcntl` | `fd, cmd, arg` | Uygulanan descriptor flag/dup komutlarının bounded alt kümesini yönetir; `FD_CLOEXEC` ve `F_DUPFD*` benzeri yollar desteklenir. Tam Linux `fcntl` komut kümesi değildir. |
| 74 | `fsync` | `fd` | Descriptor’ın desteklediği backend’de veriyi/metadata’yı eşzamanlar. Persistent NPKFS için sınırlı commit/readback yolu vardır. |
| 75 | `fdatasync` | `fd` | NPKernel’de fd sync handler’ına yönlenir; uygulanan backend’in sınırları `fsync` ile aynıdır. |
| 79 | `getcwd` | `buf, size` | Mutlak cwd’yi NUL ile kopyalar; başarıda NUL hariç yol uzunluğunu döndürür, küçük buffer için `-ERANGE`. |
| 80 | `chdir` | `path` | Process cwd’sini var olan bir dizine ayarlar. Relative yol çözümü cwd’ye göredir; genel mount/permission namespace değildir. |
| 89 | `readlink` | `path, buf, size` | Şu an genel symlink ağacı değil; `/proc/self/exe` için son image yolunu bounded biçimde verir. Dönüş NUL dahil olmayan kopyalanmış byte sayısıdır. |
| 114 | `waitpid` | `pid, status, options` | Proje uyumluluk alias’ıdır; dispatcher `wait4` handler’ına yönlendirir. Tam bağımsız Linux waitpid semantiği olarak yorumlanmamalıdır. |
| 162 | `sync` | — | VFS’in desteklenen kaynakları için global sync çağrısı yapar. |
| 217 | `getdents64` | `fd, dirent_buf, count` | Açık dizinden bounded Linux-benzeri `dirent64` kayıtları yazar; tek istekte en fazla 4096 byte kabul edilir. |
| 257 | `openat` | `dirfd, path, flags, mode` | `AT_FDCWD` ile cwd-relative veya desteklenen directory descriptor temelinde bounded open. Tam `openat2`/path resolution semantiği yoktur. |

## 3. Bellek ve zaman

| Numara | Ad | Argümanlar | Kısa özet ve kapsam |
|---:|---|---|---|
| 9 | `mmap` | `addr, length, prot, flags, fd, offset` | Anonymous lazy mapping; immutable initramfs regular file için bounded `MAP_PRIVATE`; NPK shared object ve kontrollü display mapping yolları. `PROT_WRITE|PROT_EXEC` reddedilir. File-backed mutable shared mapping ve kernel-private flag’ler kullanıcı ABI’si değildir. |
| 10 | `mprotect` | `addr, length, prot` | VMA/page izinlerini günceller; W^X korunur. |
| 11 | `munmap` | `addr, length` | Kullanıcı mapping’ini kaldırır, ilgili VMA/backing referanslarını temizler. |
| 12 | `brk` | `new_break` | Process heap sonunu bounded biçimde sorgular/günceller; allocator semantiği kullanıcı runtime’ına aittir. |
| 35 | `nanosleep` | `request, remainder` | Süreyi kernel tick’leri üzerinden bekler; gelişmiş clock/timer ve tam Linux interruption/remainder semantiği garanti edilmez. |
| 63 | `uname` | `utsname` | Sabit NPKernel sistem kimliğini (sysname `NPKernel`, nodename `npk`, release `0.2.0-dev`, machine `x86_64`) kullanıcı yapısına yazar. |
| 228 | `clock_gettime` | `clock_id, timespec` | Desteklenen clock kaynağından saniye/nanosaniye değerini userspace’e yazar. Tüm Linux clock id’leri ve vDSO sağlanmaz. |

`mmap` için izin bitleri ve temel flag’ler `include/npk/user_abi.h` içindeki `NPK_USER_PROT_*` ve `NPK_USER_MAP_*` sabitleridir. Anonymous sayfalar ilk erişimde oluşturulabilir; fork sonrası writable sayfalar COW ile paylaşılır. File-backed map yalnızca belgelenen immutable initramfs regular-file/`MAP_PRIVATE` alt kümesidir.

## 4. Process, thread ve scheduler

| Numara | Ad | Argümanlar | Kısa özet ve kapsam |
|---:|---|---|---|
| 24 | `sched_yield` | — | Mevcut scheduler’a yield eder. Scheduler/workload BSP üzerinde çalışır; tam per-CPU SMP scheduler yoktur. |
| 39 | `getpid` | — | Mevcut process PID’sini döndürür. |
| 56 | `clone` | `flags, child_stack, parent_tid, child_tid, tls` | Thread/process oluşturmanın sınırlı alt kümesi. Kabul edilen kombinasyonlar `CLONE_VM`, FS/files/sighand/thread ve bounded TID/TLS flag’leriyle sınırlıdır; `clone3`, tüm Linux flag’leri ve tam pthread semantiği yoktur. |
| 57 | `fork` | — | Child process’i COW address-space ile oluşturur; parent’a child PID, child’a 0 dönüşü hedeflenir. |
| 59 | `execve` | `path, argv, envp` | VFS’ten bounded ELF image yükleyerek mevcut process’i değiştirir. ET_EXEC ve kontrollü ET_DYN/PT_INTERP yolu vardır; dynamic linker relocation/dependency çözümü kernel tarafından yapılmaz. |
| 60 | `exit` | `status` | Mevcut thread’i sonlandırır; normal syscall dönüşü yoktur. |
| 61 | `wait4` | `pid, status, options` | Child süreç durumunu bekler/toplar; uygulanan seçenekler ve status copyout davranışı bounded’dir, `rusage` argümanı şu an dispatcher’dan aktarılmaz. |
| 62 | `kill` | `pid, signal` | Process’e signal gönderir; temel signal delivery ve izin/target kuralları vardır. |
| 114 | `waitpid` | `pid, status, options` | Proje alias’ı; aynı `sys_wait4` handler’ına gider. |
| 158 | `arch_prctl` | `operation, address_or_value` | `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS`, `ARCH_GET_GS` ile thread’in FS/GS base’ini ayarlar veya sorgular. |
| 186 | `gettid` | — | Geçerli thread TID’sini döndürür. |
| 218 | `set_tid_address` | `tidptr` | Thread çıkışındaki clear-child-tid/futex wake mekanizması için adres kaydeder. |
| 231 | `exit_group` | `status` | Thread group’unu sonlandırır; normal syscall dönüşü yoktur. |
| 234 | `tgkill` | `tgid, tid, signal` | Belirtilen thread’e, thread group doğrulamasıyla signal gönderir. |

`clone` destekli flag sabitleri `NPK_USER_CLONE_*` adıyla ABI başlığındadır. Kernel v1 tam pthread runtime’ı, robust/PI futex’leri, tüm clone varyantlarını veya AP scheduler migration’ını vaat etmez.

## 5. Signal ve bekleme primitive’leri

| Numara | Ad | Argümanlar | Kısa özet ve kapsam |
|---:|---|---|---|
| 13 | `rt_sigaction` | `signal, act, oldact, sigsetsize` | Process başına signal action kaydeder/sorgular; handler delivery’nin bounded alt kümesi vardır. |
| 14 | `rt_sigprocmask` | `how, set, oldset, sigsetsize` | Thread/process signal maskesini desteklenen bitset kurallarıyla değiştirir; pending signal delivery ile bütünleşir. |
| 15 | `rt_sigreturn` | — | Kernel’in kurduğu user signal frame’inden önceki register/syscall bağlamını geri yükler; uygulama tarafından gelişigüzel çağrılacak normal dönüş API’si değildir. |
| 129 | `rt_sigqueueinfo` | `pid, signal, siginfo` | Process’e bounded realtime signal ve `siginfo_t`/`si_value` payload’ı kuyruğa alır; kuyruk dolarsa `-EAGAIN`. |
| 202 | `futex` | `uaddr, op, val, timeout, uaddr2, val3` | `FUTEX_WAIT`/`FUTEX_WAKE` ve private flag’in bounded alt kümesi; timeout ve signal interruption olabilir. PI, requeue, bitset ve robust-list işlemleri yoktur. |
| 232 | `epoll_wait` | `epfd, events, maxevents, timeout_ms` | Bounded epoll watch set’inden hazır eventleri toplar; level-triggered alt kümedir. |
| 233 | `epoll_ctl` | `epfd, op, fd, event` | Epoll watch ekler/değiştirir/siler; uygulanmış operation ve event maskeleriyle sınırlıdır. |
| 271 | `ppoll` | `fds, nfds, timeout, sigmask, sigsetsize` | Poll readiness’ini timespec timeout ve bounded signal maskesiyle bekler. |
| 291 | `epoll_create1` | `flags` | Epoll descriptor’ı oluşturur; sadece desteklenen flags kabul edilir. |
| 297 | `rt_tgsigqueueinfo` | `tgid, tid, signal, siginfo` | Tek thread’e bounded realtime signal ve payload kuyruğa alır. |

Realtime `sigwait`/`rt_sigtimedwait`, tam nested signal frame semantiği ve tüm Linux signal flag’leri mevcut kapsamda değildir. `poll`/`epoll` regular file, pipe ve TTY readiness’ine yöneliktir; blocking pipe wait queue, `select` ve edge-triggered epoll yoktur.

## 6. NPKernel’e özgü extension syscall’ları

Aşağıdaki `0x400`–`0x407` numaraları **Linux syscall numaraları değildir**; NPKernel extension namespace’idir.

| Numara | Ad | Argümanlar | Kısa özet |
|---:|---|---|---|
| `0x400` | `NPK_SYS_NPK_SHM_CREATE` | `size` | Sıfırlanmış anonymous shared-memory object’i descriptor olarak oluşturur; boyut en fazla 256 × 4096 byte’dır. `mmap(..., MAP_SHARED, fd, ...)` ile paylaşılır. |
| `0x401` | `NPK_SYS_NPK_FD_SEND` | `target_pid, source_fd` | Yetki/parent-child sınırı içindeki canlı process’e descriptor referansı gönderir. Pending queue process başına bounded’dir. |
| `0x402` | `NPK_SYS_NPK_FD_RECV` | — | Gelen descriptor referansını yeni fd olarak alır; kuyruk boşsa kernel’in hata dönüşü kullanılır. |
| `0x403` | `NPK_SYS_NPK_DISPLAY_INFO` | `user_info` | Kernel GOP framebuffer metadata’sını ve mapping ölçülerini kullanıcı yapısına kopyalar. |
| `0x404` | `NPK_SYS_NPK_DISPLAY_CLAIM` | — | Tek sahibi olan display lease’i alır; GUI/compositor değildir. |
| `0x405` | `NPK_SYS_NPK_DISPLAY_MAP` | `hint, flags` | Yalnızca lease sahibine kernel’in seçtiği GOP aralığını map eder; kullanıcı fiziksel adres/uzunluk seçemez. |
| `0x406` | `NPK_SYS_NPK_DISPLAY_RELEASE` | — | Display mapping’lerini revoke edip lease’i bırakır. |
| `0x407` | `NPK_SYS_NPK_INPUT_READ` | `buffer, capacity_bytes` | Display lease sahibine bounded structured keyboard/mouse event kayıtları verir; text stdin’in yerine geçmez. |

Extension yapıları, event type’ları, display metadata ve sabitler `include/npk/user_abi.h` içinde tanımlıdır. Input event kaydı 24 byte’tır; queue ve owner kuralları ABI başlığındaki kernel sözleşmesine tabidir.

## 7. Önemli sınırlar ve kaynak doğruluğu

- Buradaki “uygulanıyor” ifadesi dispatcher’da bir handler’a bağlandığını belirtir; her Linux flag/edge case’i kapsadığı anlamına gelmez.
- Path çözümü bounded’dir: cwd-relative yollar, initramfs hiyerarşisi ve belirtilen persistence yolları çalışır; genel mount/permission namespace veya genel symlink graph’ı yoktur. `readlink` genel symlink desteği değildir.
- `open`, `stat`, `execve` ve descriptor davranışlarını gerçek Linux filesystem varsayımlarıyla genişletmeyin; VFS/backend sınırlamalarını dikkate alın.
- `clone`, futex, signal, poll/epoll ve persistent storage için TODO ve verification belgelerindeki bounded kapsam geçerlidir. Tam glibc/musl, GNU Bash, pthread, network, socket ya da device namespace uyumluluğu iddia edilmez.
- İsim veya numara `include/npk/syscall.h`, gerçek dispatch `src/syscall/syscall.c` ile belirlenir. Bu doküman koddan farklı görünürse kaynak kodu ve ABI başlığını esas alıp dokümanı güncelleyin.

## 8. İlgili kaynaklar

- [`include/npk/syscall.h`](../include/npk/syscall.h) — kernel syscall numaraları ve frame tipi.
- [`include/npk/user_abi.h`](../include/npk/user_abi.h) — userspace ABI sabitleri ve veri yapıları.
- [`src/syscall/syscall.c`](../src/syscall/syscall.c) — handler’lar ve gerçek dispatcher.
- [`docs/user-abi.md`](user-abi.md) — ELF başlangıcı, TLS, memory, process, TTY, display/input sözleşmeleri.
- [`README.md`](../README.md) — proje özeti ve uygulanan syscall aileleri.
- [`TODO.md`](../TODO.md) ve [`docs/verification.md`](verification.md) — açık sınırlar ve test kanıtları.
