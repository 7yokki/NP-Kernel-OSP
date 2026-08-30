# Signal, VM ve FD altyapısı — güncel durum

**Belge durumu:** Uygulama fazı tamamlandı; bu dosya artık ön-uygulama planı değil, gerçekleşen kernel değişikliklerinin kısa teknik kaydıdır.

## Tamamlanan implementasyon

`rt_sigaction` artık yalnızca action kaydetmez. Process başına action tablosu fork sırasında miras alınır; kullanıcı fault’u veya `kill`/`tgkill` ile gelen sinyal için kayıtlı handler varsa user stack üzerinde doğrulanan minimal signal frame kurulur. Handler dönüşü `rt_sigreturn` ile özgün syscall/IRET frame’ini ve önceki signal maskesini geri yükler.

Signal durumu thread başına tutulur. `signal_blocked` ve `signal_pending` 64-bit coalescing bitset’leridir. `SIG_BLOCK`, `SIG_UNBLOCK` ve `SIG_SETMASK` işlemleri Linux x86_64 syscall numarası `14` üzerinden uygulanır. Handler’a girerken gelen sinyal ve action maskesi geçici olarak blocked maskesine eklenir; maskeli sinyaller pending bitset’te tutulur ve unblock sonrasında yeniden değerlendirilir. Aynı non-realtime sinyalin tekrarı tek pending bit olarak birleşir. `SIGKILL` ve `SIGSTOP` maskelenemez veya handler ile yakalanamaz. Realtime sinyaller için bounded FIFO queue, `siginfo_t`/`si_value` payload, `SA_SIGINFO` frame ve queue overflow (`EAGAIN`) desteği eklenmiştir; nested frame ve sigwait ailesi hâlâ sınırlıdır.

`kill(pid, signal)` ve `tgkill(tgid, tid, signal)` hedef process/thread tablolarına bağlanmıştır. `signal == 0` existence probe olarak çalışır; geçersiz hedef veya signal numarası Linux errno sözleşmesine göre reddedilir. Default action kullanan fatal sinyaller yalnızca hedef process’i sonlandırır; kernel panic yolu kullanıcı fault’larından ayrıdır.

`mprotect` VMA’ları page-aligned sınırlar üzerinden böler, lazy mapping metadata’sını günceller, mevcut user PTE izinlerini değiştirir ve TLB’yi invalidate eder. W^X kontrolü runtime koruma değişikliklerinde de uygulanır; `PROT_WRITE | PROT_EXEC` reddedilir.

VFS descriptor modeli global çıplak fd listesi yerine refcount’lu open-handle katmanına geçirilmiştir. Process-local logical fd slotları aynı open handle’ı paylaşabilir; `fork`, `dup`, `dup2` ve `fcntl(F_DUPFD*)` refcount’u ve ortak file offset’i korur. Pipe, bounded nonblocking ring-buffer backend’i olarak eklenmiştir; boş pipe `-EAGAIN`, okuyucusu kalmayan pipe’a yazma `-EPIPE` verir. Descriptor flags içinde `FD_CLOEXEC` tutulur.

## Doğrulama kanıtı

| Yol | Kanıt |
|---|---|
| Signal delivery | SIGILL handler + `UD2` smoke testinde handler çalıştı ve `exit(0)` ile process temiz çıktı. |
| Process signal ve mask | `docs/qemu-signal-mask-smoke-2.log` içinde syscall sırası `13`, `14`, `39`, `62`, ikinci `14`, `15`, `60` olarak görüldü; blocked sinyal unblock sonrasında handler’a ulaştı. |
| `mprotect` | ABI smoke testinde `PROT_READ|PROT_EXEC` başarılı, W^X’i ihlal eden `PROT_WRITE|PROT_EXEC` çağrısı `-EINVAL` ile reddedildi. |
| Pipe/fd | `pipe`, `write`, `dup2`, `close`, `read`, `fcntl(F_DUPFD)` yolları `docs/qemu-abi-smoke-2.log` içinde başarılı tamamlandı. |
| Üretim boot | Demo kapalı temiz build sonrası `docs/final-production-bios.log` ve `docs/final-production-uefi.log` içinde `NPKernel initialization complete; interrupts enabled` görüldü; panic, page fault veya beklenmeyen exception yok. |

## Bilinçli kalan kapsam

Futex çekirdekte bounded bir waiter tablosuyla uygulanmıştır: address-space anahtarlı `FUTEX_WAIT`/`FUTEX_WAKE`, timeout, signal interruption ve `clear_child_tid` wake yolu vardır. Clone yolu TID/TGID, `set_tid_address`, `exit_group`, group-wide exit ve sınırlı shared FS/files flag’leriyle bounded thread-group desteği verir; robust-list, PI/requeue/bitset futexleri ve tüm pthread/Rust runtime sözleşmesi kapsam dışıdır. Pipe backend’i nonblocking’dir; `poll`/`ppoll`/level-triggered `epoll` artık regular file, pipe ve TTY readiness için vardır, ancak blocking pipe wait queue ve edge-triggered epoll yoktur.

ELF loader bounded dynamic handoff uygular: `PT_INTERP` yolu doğrulanır, VFS’den okunur, güvenli ve çakışmayan bias ile ikinci ET_DYN image olarak aynı address-space’e map edilir ve `AT_PHDR`/`AT_PHNUM`/`AT_PHENT`/`AT_BASE`/`AT_ENTRY` auxv kayıtları hazırlanır. `DT_NEEDED` grafı, sembol çözümleme ve relocation userspace `ld.so` sorumluluğundadır; gerçek glibc/musl runtime handoff testi kapsam dışıdır. Adversarial corpus host harness’i 38 vakayı 0 hata ile doğrulamaktadır.
