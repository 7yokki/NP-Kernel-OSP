# NPKernel userspace güvenlik denetimi — başlangıç snapshot’ı

> **Tarihsel belge uyarısı:** Bu dosya ilk tasarım aşamasındaki güvenlik risklerini kaydeder. Satır 11–18’deki “eksik”, “yok” ve “gerçek değil” ifadeleri başlangıç snapshot’ına aittir; güncel implementasyon durumu için [`verification.md`](verification.md) ve [`v0.3-audit.md`](v0.3-audit.md) belgelerine bakılmalıdır. Özellikle ELF rollback/W^X, ayrı address space, ring-3 exception isolation, signal delivery, `mprotect`, pipe ve refcount’lu fd altyapıları sonradan tamamlanmıştır.

## Hedef platform

Hedef `x86_64`, Limine native protocol, BIOS/UEFI boot, higher-half kernel, freestanding C17/NASM ve QEMU doğrulamasıdır. Kernel’de red zone kapalıdır. Bu çalışma tek CPU ve QEMU öncelikli ilerleyecek; gerçek donanım iddiası ancak ayrıca doğrulama yapıldığında kurulacaktır.

## Doğrudan gözlemler

| Bileşen | Mevcut durum | Güvenlik/işlev riski |
|---|---|---|
| ELF64 | Header ve PT_LOAD doğrulaması var; sayfalar aktif CR3’e map ediliyor | Ayrı kullanıcı adres alanı yok, rollback yok, overlap/align/entry/stack doğrulaması eksik, user pointer güvenliği yok, exec ve ring-3 dönüşü yok |
| VMM | `vmm_map_page` yalnızca mevcut CR3 üzerinde çalışıyor | Address-space ownership yok; page-table intermediate permission’ları güvenli değil; unmap fiziksel sayfayı serbest bırakmıyor; page fault/mmap bölge modeli yok |
| GDT | Ring-0/Ring-3 code-data descriptor’ları var | TSS, RSP0, IST ve per-thread kernel stack yok |
| IDT/entry | Varsayılan exception ve tek keyboard IRQ; bootstrap syscall stub | Syscall frame’i header ile uyuşmuyor; user RSP/SS saklama ve güvenli SYSRET/IRETQ dönüşü yok; page fault recovery yok |
| Process | PID/TID kaydı ve placeholder scheduler var | Gerçek context switch, CR3 geçişi, user trap frame’i ve process cleanup yok |
| Syscall | Küçük Linux x86_64 numara tablosu | Kernel pointer’larına doğrudan güveniyor; `mmap`, `munmap`, `brk`, `execve`, `/proc`, timer ve dosya syscall’ları gerçek değil |
| IRQ/timer | PIC ve IRQ1 keyboard var | Timer interrupt ve preemption yok; scheduler zaman tabanlı çalışmıyor |
| Storage | Initramfs CPIO read-only VFS var | ATA/block layer/disk persistence yok |

## Tehdit modeli

Kullanıcı programı **kötü niyetli veya hatalı** kabul edilir. Userspace hiçbir pointer, uzunluk, string sonlandırması, canonical adres veya page permission koşulunu garanti etmez. Kernel; buffer taşması, integer wrap, page-table permission yükselmesi, use-after-free, stale mapping, syscall reentrancy, invalid ELF, segment çakışması, stack taşması ve I/O timeout durumlarında çökmemeli veya kernel belleğini kullanıcıya açmamalıdır.

İlk güvenlik sınırı tek CPU/QEMU’dur. SMP, DMA/IOMMU ve gerçek cihazlarda cache coherency bu iterasyonun dışında tutulacak; ATA PIO kullanılacağı için DMA kaynaklı risk azaltılacaktır. Disk imajı yalnızca QEMU test cihazı kabul edilecek; yazma desteği eklenirse sektör aralığı ve timeout kontrolleri zorunlu olacaktır.

## Uygulama kabul kriterleri

1. ELF yalnızca doğrulanmış, canonical, kullanıcı alanı aralığında, page-aligned ve overflow-safe PT_LOAD segmentlerini kabul eder.
2. Her process kendi PML4 köküne, kernel mapping’lerinin kontrollü paylaşımına ve kullanıcı mapping ownership bilgisine sahip olur.
3. TSS RSP0 ve ayrı kernel stack kurulmadan ring-3 çalıştırma etkinleştirilmez.
4. Syscall/interrupt girişleri kullanıcı register’larını tam trap frame ile korur; dönüşte CS/SS/RIP/RSP/RFLAGS doğrulanır.
5. `mmap`, `munmap`, `brk`, `execve`, `/proc` ve timer syscall’ları gerçek kernel veri yapıları üzerinden çalışır; desteklenmeyen yollar açıkça `-ENOSYS` döndürür.
6. ATA sürücüsü PIO polling, status/error kontrolü, sector-count sınırı ve timeout ile test edilir; sonsuz bekleme kabul edilmez.
7. Her aşama `make check`, seri log ve QEMU test imajı ile tekrarlanabilir biçimde doğrulanır.
