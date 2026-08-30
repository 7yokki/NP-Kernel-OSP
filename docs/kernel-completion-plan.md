# NPKernel genişletilmiş kernel planı

Bu belge, mevcut kontrollü static-ELF ve tek-CPU çekirdeği, kullanıcının son talep ettiği kernel-side özelliklere doğru genişletmek için kabul edilen sözleşmeleri tanımlar. Hedef, Linux x86_64 ABI’nin ilgili çekirdek yollarını güvenli ve bounded bir subset olarak gerçeklemek; kapsam dışı davranışları sessizce başarılı saymamak ve her yeni ABI yolunu QEMU/regresyon kapısıyla doğrulamaktır.

## 1. Dynamic ELF yürütme

Kernel, `PT_INTERP` içeren ana ELF’yi reddetmek yerine interpreter yolunu doğrular, root VFS üzerinden açar ve aynı address-space içinde ikinci bir ET_DYN image olarak map eder. Kernel’in görevi interpreter’ın PT_LOAD segmentlerini güvenli biçimde kurmak ve Linux başlangıç sözleşmesini sağlayan auxv kayıtlarını (`AT_PHDR`, `AT_PHNUM`, `AT_PHENT`, `AT_BASE`, `AT_ENTRY`, `AT_PAGESZ`, `AT_RANDOM` için bounded değer) oluşturmaktır. `DT_NEEDED` grafiğinin, sembol versiyonlamasının ve PLT/GOT relocation’larının genel çözümü kullanıcı-space dynamic linker’a bırakılır; kernel bunları kendisi uygulayıp sessizce yanlış sembol bağlamaz.

Interpreter’ın kendisi de static-ELF loader’ın aynı overflow, segment-overlap, user-range, W^X ve rollback kontrollerinden geçer. `PT_INTERP` string’i NUL-terminated ve bounded olmalıdır; yalnızca absolute `/` yolu kabul edilir ve VFS dışına çıkış reddedilir. Interpreter bulunamazsa `ENOENT`, bozuk veya desteklenmeyen dynamic metadata varsa `ENOEXEC` döndürülür. Böylece dynamic ELF desteği, interpreter’ın initramfs veya persistent VFS üzerinde gerçekten bulunmasına bağlı, açıkça test edilebilir bir sözleşmeye sahip olur.

## 2. Thread-group ve runtime semantiği

`process_t` bir Linux thread group’ın address-space/file/signal owner’ı, `thread_t` ise tek bir TID olarak ele alınır. Her thread aynı `mm` root’unu paylaşır; group leader PID/TGID olarak kalır. `clone` için `CLONE_VM`, `CLONE_THREAD`, `CLONE_SIGHAND`, `CLONE_FILES`, `CLONE_FS`, `CLONE_SETTLS`, `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID` ve `CLONE_CHILD_CLEARTID` alanları doğrulanır. Thread çıkışı yalnızca kendi TID’sini zombie yapar; `exit_group` ise group içindeki tüm thread’leri atomik biçimde sonlandırır, clear-child-tid/futex wake işlemlerini yapar ve process wait4 durumunu bir kez uyandırır.

Thread-local signal maskesi ve pending queue thread başına, action tablosu process/thread-group başına tutulur. `tgkill` TID’yi aynı TGID içinde arar; `kill` group leader ve process-group kurallarına göre hedef seçer. `procfs` Threads alanı canlı thread sayısını gösterir. `futex` anahtarı shared address-space root ve kullanıcı adresinden üretilir; thread çıkışında clear-child-tid atomik zero+wake yolu korunur.

## 3. SMP

Limine MP/SMP response boot contract’a eklenmiştir. BSP ve her başlatılan AP için ayrı GDT, TSS, IST stack, kernel stack ve GS tabanlı CPU-local alan kurulmaktadır; AP callback’i kendi IDT’sini ve SYSCALL MSR’lerini de yüklemeden `online` sayılmaz. Eksik veya hatalı MP response tek CPU fallback’i ile boot’u bozmaz.

Bu sürümde **SMP scheduler henüz açılmamıştır**: process/thread tabloları, `current` pointer’ı, scheduler tick’i ve syscall current-process durumu hâlâ BSP-globaldir. PIT/IRQ0 routing, APIC/IOAPIC, IPI, spinlock’lu global tablolar, CPU-local run queue ve thread migration uygulanmadan AP’ler yalnızca kesmeleri kapalı güvenli idle döngüsünde tutulur. Bu nedenle `smp_enabled()` MP bring-up’ın mevcut olduğunu ifade eder; paralel userspace/thread execution veya tam SMP scheduler garantisi vermez.

Güvenli SMP kapısı QEMU `-smp 2` altında AP discovery, per-CPU arch initialization’ın tamamlanması, iki CPU’nun online sayılması, kernel init’in sürmesi ve panic/fatal marker bulunmamasıdır. Tam SMP desteği için yukarıdaki scheduler/interrupt-routing katmanları ayrı bir release blocker olarak kalır.

## 4. poll/epoll

VFS descriptor için readiness maskesi tanımlanır: regular file/directory her zaman okunabilir, pipe read end veri veya EOF durumunda okunabilir, pipe write end reader varsa ve kapasite varsa yazılabilir. `poll(2)` bounded `pollfd` dizisini copyin/copyout ile işler ve timeout’u PIT tick’e çevirir. `epoll_create1`, `epoll_ctl` ve `epoll_wait` ayrı kernel epoll object ve bounded watch tablosu kullanır. İlk implementasyon descriptor event state değiştiğinde scan ile hazırları toplar; callback/wait-queue optimizasyonu daha sonra yapılabilir. Unsupported descriptor flag’leri `EINVAL`, taşan fd/watch sayıları `EMFILE` veya `ENOMEM` ile sonuçlanır.

## 5. Persistent filesystem

ATA PIO’nun mevcut bounded 28-bit LBA backend’i korunur; üzerine küçük, little-endian, sabit-bölgeli NPKFS eklenmiştir. İki superblock/inode metadata slotu CRC32 ile doğrulanır; tek transaction sector’ündeki prepare/clear marker ve generation karşılaştırması, metadata yazımının yarıda kalması halinde geçerli slotun seçilmesini sağlar. Dosya başına 128 sektör direct data alanı, 64 inode ve bounded isim/size/sector kontrolleri vardır. **Data block’ları için ayrı checksum veya copy-on-write sürümleme yoktur**; bu nedenle protokol metadata bütünlüğünü ve yarım metadata commit’ini kapsar, sessiz data-bit rotasyonunu tespit etmeyi garanti etmez.

Mount sırasında magic, version, geometry, checksum, inode checksum ve data-sector sınırları doğrulanır. Yazma işlemi data block → backup inode/superblock → primary inode/superblock → clear marker sırasını izler; geçerli slotlardan daha yüksek generation seçilir. VFS descriptor open-file object’e bağlı inode/offset katmanını kullanır; initramfs ve NPKFS aynı üst VFS API’sini paylaşır. `fsync`, `fdatasync`, `sync` ve close metadata commit kapısına bağlanmıştır.

Disk imajı formatlama Makefile hedefinde açıkça yapılır. Boş veya bozuk metadata, kernel’in initramfs fallback yolunu engellemez; iki slot da geçersizse bounded disk bölgesi yeniden formatlanır. Write/fsync, reboot/remount/readback, tek slot bozulması ve iki slot bozulması sonrası recovery testleri release kayıtlarına eklenmiştir. Güç kesintisi simülasyonu gerçek reset/sector fault injection değil, on-disk metadata bozulması ve geçerli-slot seçimi testidir.

## 6. Realtime signals

Standart 1–64 bit pending maskesine ek olarak bounded realtime queue tutulur. Her queued entry signal number, `siginfo` payload ve sender kimliğini taşır. Yalnızca `SIGRTMIN..SIGRTMAX` için queue kullanılır; teslimat düşük signal numarası önceliğiyle, aynı signal için FIFO sırasıyla yapılır. Process veya thread hedefi, `SA_SIGINFO` üç argümanlı handler kurulumu ve `rt_sigreturn` sırasında frame doğrulaması uygulanır. Queue dolduğunda `EAGAIN` dönülür; standart non-realtime sinyaller queue’ya alınmaz ve pending bit üzerinden coalesce olur. Blocked sinyal queue’da kalır, unblock veya syscall return boundary’de teslim edilir. SIGKILL/SIGSTOP maskelenemez ve queue’ya konmaz.

## 7. Adversarial ELF corpus

Corpus; truncated header/program table, invalid class/data/version/machine, phnum/phentsize overflow, filesz>memsz, offset+filesz overflow, invalid align, misaligned vaddr/offset, user-range wrap, overlapping page ranges, W+X, entry outside executable PT_LOAD, excessive page count, duplicate veya malformed PT_INTERP, unterminated/relative/traversal interpreter path ve malformed PT_DYNAMIC vakalarını içerir. Her corpus case için beklenen boolean kaydedilir; loader hiçbir case’te page leak, VMA leak veya address-space root leak bırakmamalıdır. Dynamic relocation compatibility bu validator corpusunun kapsamı değildir.

## Release ölçütü

Her başlık için source-level gate tek başına yeterli değildir. Build, static ve bounded dynamic-ELF handoff smoke, clone/futex/thread-group, `poll`/`epoll`, persistent disk mount/write/readback/recovery, realtime `sigqueueinfo`, SMP `-smp 2` ve adversarial corpus sonuçları ayrı ham log dosyalarında tutulur. Desteklenmeyen ileri semantik `-ENOSYS` veya ilgili Linux errno’su ile reddedilir; başarılı görünmek için sessizce no-op yapılmaz.

## Kaynaklar

[1]: https://refspecs.linuxfoundation.org/elf/gabi4+/contents.html "System V ABI ELF specification"
[2]: https://man7.org/linux/man-pages/man2/execve.2.html "Linux execve(2)"
[3]: https://man7.org/linux/man-pages/man2/clone.2.html "Linux clone(2)"
[4]: https://man7.org/linux/man-pages/man2/poll.2.html "Linux poll(2)"
[5]: https://man7.org/linux/man-pages/man7/signal.7.html "Linux signal(7)"
[6]: https://github.com/limine-bootloader/limine/blob/v9.x-branch/PROTOCOL.md "Limine boot protocol"

