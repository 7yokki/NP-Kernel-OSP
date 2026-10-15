# NPKernel — sonraki ajan için devir notu

**Güncelleme:** 2026-10-14  
**Amaç:** Bu dosyayı okuyup projeyi yeni açan bir ajan, eski sohbeti istemeden mevcut kaynak durumunu ve kullanıcı önceliklerini anlayarak devam edebilsin. Bu bir repo durum özeti ve çalışma sözleşmesidir; kaynak kodun yerine geçmez.

## 1. Projenin hedefi ve kullanıcı tercihi

NPKernel, gelecekteki NPOS işletim sisteminin **x86_64 monolitik ama modüler kernel tabanıdır**. Amaç bir kerede Linux’u kopyalamak veya erken sürümde her özelliği kusursuz yapmak değil; küçük ve kullanılabilir bir ilk sürüm için gereken zorunlu kernel temellerini sağlamlaştırıp sonra güvenlik düzeltmeleri, sürücüler ve kullanıcı alanı özelliklerine geçmektir.

Kullanıcı özellikle şu tasarım ayrımını önemsiyor:

- Shell kernel’e gömülü bir `kernel.c` konsolu/komut yorumlayıcısı **olmamalı**.
- Kernel terminal yüzeyini hazırlar, ekranı temizler ve VFS’den `bash.elf` gibi bir userspace ELF’i başlatır. Shell’in komut işleme mantığı userspace’te kalır.
- Userspace programı “şu an input yok” diye sürekli poll/sleep etmek zorunda bırakılmamalı. Terminal `read(0, ...)` sözleşmesi normal terminal gibi bekleyebilmeli; bu kernel/TTY kontratıdır.
- USB tarafında önce cihaz tanılama ve sınıflandırma var. “Cihaz klavye olarak tanındı” demek, USB HID’den gerçek tuş girdisi alınıyor demek değildir.
- İlk sürüm için mükemmel Linux uyumluluğu veya tam SMP zorunlu değil; kapsam ve sınırlamalar açıkça belirtilmeli, desteklenmeyen işlevler sahte başarı vermemeli.

Kullanıcı Türkçe yazıyor; varsayılan yanıt dili Türkçe olsun.

## 2. Ajanın nasıl çalışması bekleniyor

1. **Bu dosyayı ve ardından** `README.md`, `TODO.md`, `docs/kernel-completion-plan.md`, `docs/verification.md`, `docs/device-manager.md`, `docs/usb-xhci-design.md` dosyalarını oku. İlgili kodu değiştirmeden önce çağrı akışını ve testleri incele.
2. Sohbet geçmişi yok diye işi başa sarma. Mevcut kaynakları ve test loglarını önce kontrol et; önceki değişiklikleri silme veya yeniden yazma.
3. Kullanıcı yönü net verdiyse rutin, düşük riskli ve geri alınabilir işleri o kapsamda tamamla; tekrar tekrar izin sorma. Sonucu kanıtla. Ürün davranışını, kapsamı veya izinleri önemli ölçüde değiştirecek bir karar eksikse önce sor.
4. **Kanıt ile yorumu ayır.** Build, QEMU, seri log veya gerçek donanım testinden geçmeyen işi “çalışıyor/test edildi” diye sunma. QEMU sonucu fiziksel donanım sonucu değildir.
5. Kernel kodunda doğruluk ve güvenlik; sonra doğrulanabilirlik ve okunabilirlik önceliklidir. Bounded buffer, integer overflow, pointer/copyin-copyout, interrupt reentrancy, SMP yarışları, DMA/cache, teardown ve hata dönüşlerini kontrol et.
6. Yeni özellik için küçük bir dikey dilim, host testi ve QEMU seri testi tercih et. Firmware/cihaz davranışını gerektiğinde üretici/standart dokümanıyla doğrula; register/TRB değerleri uydurma.
7. Büyük/refaktör niteliğindeki değişiklik öncesi kaynak kopyası/zip checkpoint’i al. Bu snapshot’ta `.git` metadata’sı yok; `git status`/`git diff` çalışacağını varsayma. Değişen dosyaları ve test sonuçlarını kendi raporunda açıkça listele.
8. Release derlemesinde `NPK_ENABLE_USER_DEMO=0` kalsın. Ring-3 smoke için geçici olarak açılırsa testten sonra varsayılan üretim ayarını yeniden kur ve bunu logla.
9. Her işi kapatırken: ne değişti, hangi testler geçti/kaldı, ne **henüz** yapılmadı, en küçük sonraki adım nedir — kısa ve açık yaz.

OSDev hata analizinde doğrudan gözlemleri hipotezlerden ayır; gerektiğinde `/home/ubuntu/skills/osdev-mentor/SKILL.md` yönergelerine uy.

## 3. Mimari harita

- **Platform/build:** x86_64, Limine v9.x-binary, BIOS ve UEFI/GOP boot, C17 freestanding + NASM, Clang/LLD. Build `Makefile` ile yapılır; kaynaklar `boot/`, `arch/x86_64/`, `src/`, public API’ler `include/npk/` altındadır.
- **Entry/arch:** `arch/x86_64/entry.asm`, `src/arch/{gdt,idt,irq,timer}.c`; GDT/TSS/RSP0/IST, ring 0/3, 256 IDT gate, syscall/interrupt frame’leri.
- **CPU/SMP:** `src/smp.c`; Limine MP response ile CPU discovery ve AP bring-up. Per-CPU descriptor/stack setup vardır. QEMU `-smp 2` testinde iki CPU online olur; **AP’ler güvenli idle’da kalır, process scheduler/iş yükü BSP’dedir.** Per-CPU run queue, IPI/APIC timer routing ve migration yok.
- **Memory:** `src/memory/{pmm,vmm,vm,heap}.c`; refcount’lu 4 KiB PMM, root-aware 4-level paging, user-range/copyin/copyout, COW, lazy anonymous/file-backed mapping, `mprotect`, `munmap`, `brk`, heap.
- **Process/ELF/syscall:** `src/proc/process.c`, `src/exec/elf.c`, `src/syscall/syscall.c`, `arch/x86_64/entry.asm`. Ayrı address space, ELF ET_EXEC/ET_DYN ve bounded PT_INTERP handoff, fork/COW, wait/exit, bounded clone/thread-group, signals, futex WAIT/WAKE ve geniş ama sınırlı Linux x86_64 syscall subset’i bulunur. Tam Linux userspace/musl/glibc/pthread uyumluluğu yoktur.
- **VFS/storage:** `src/fs/vfs.c`, `src/fs/procfs.c`, `src/drivers/ata.c`; initramfs CPIO `newc`, bounded directory/canonical absolute paths, refcount’lu handles/fd’ler, pipe/poll/epoll, `/proc`; ATA PIO üzerinde sınırlı fixed-region `/persist` NPKFS vardır. Data journaling ve gerçek güç-kesintisi crash consistency yoktur.
- **Console/input/display:** `src/console.c`, `src/drivers/{gop,vga,font,keyboard,mouse}.c`; GOP framebuffer TTY, VGA fallback, seri log, PS/2 Türkçe-Q keyboard, structured keyboard/mouse events ve kontrollü display lease ABI’si. GUI/compositor yoktur.
- **PCI/driver catalog:** `src/drivers/pci.c`, `src/device/device_manager.c`, `src/device/device_config.c`; bounded PCI enumeration, genel device/driver registry ve DDC kategori/model çözümü.
- **USB:** `src/drivers/{xhci,usb}.c`, `include/npk/usb.h`; polling xHCI ile QEMU root-port cihazını adresleme, EP0 üzerinden device + configuration descriptor okuma ve interface kimliği çıkarma.

Detaylı syscall ve sınırlamalar güncel `README.md`, `docs/user-abi.md`, `TODO.md` ve `docs/v0.3-audit.md` içindedir. `docs/security-audit.md` **tarihsel başlangıç snapshot’ıdır**; oradaki eski “yok/eksik” iddialarını güncel gerçek sanma. Güncel kapsam için `README.md` + `docs/verification.md` + `docs/v0.3-audit.md` kullan.

## 4. Bu oturumda doğrulanmış USB/DDC durumu

- Katalog kaynağı: `user/ddc.json`. Initramfs üreticisi `tools/make_initramfs.py` bunu CPIO root’unda `ddc.json` olarak paketler; çalışma anındaki yol **`/ddc.json`**’dur.
- Kernel boot sırası: VFS mount → `devmgr_load_default_config()` → `usb_init()`/xHCI. Device Manager katalogu **VFS API’siyle** okur; device manager/USB kodu raw user klasörünü veya initramfs belleğini elle dolaşmaz.
- DDC v1’de kategori `bus + class + subclass + protocol` ile belirlenir. USB ürün/modeli ayrı `VID:PID + type` kaydıyla eşleşir. Model kaydı bulunamazsa category `generic_name` kalır; category için beklenen driver adı da katalogda tanımlanır.
- `user/ddc.json` şu anda HID boot keyboard `03/01/01`, HID keyboard `03/01/00`, mouse `03/01/02` kategorilerini tanımlar. QEMU `VID:PID 0627:0001` adı `QEMU USB Keyboard` olarak eşleşir.
- xHCI generic GET_DESCRIPTOR(type,index) yolu ile önce device descriptor, ardından configuration descriptor alır. Device descriptor class `00/00/00` olabilir; sınıf gerçek olarak interface descriptor `03/01/01`’de gelir. Alternate setting 0 interface’leri Device Manager’a ayrı USB kimlikleri olarak kaydedilir.
- QEMU q35 + `qemu-xhci` + `usb-kbd` + `-smp 2` smoke testinde `/ddc.json`, 2 CPU online, `0627:0001`, interface `03/01/01`, kategori `keyboard` ve ad `QEMU USB Keyboard` seri logda doğrulandı.
- Host testinde ürün eşleşmeyen `03/01/01` interface’in yine keyboard olarak sınıflandırıldığı ve **`Generic Keyboard`** adını aldığı doğrulandı.
- **Açık ve kritik sınır:** DDC `usb_hid_keyboard` hedef adını bulur ancak bu isimde çalışan HID keyboard driver henüz register edilmemiştir; USB keyboard interface `UNBOUND` kalır. Test gerçek klavye input’u kanıtlamaz. HID report descriptor, SET_CONFIGURATION/SET_PROTOCOL, interrupt-IN endpoint/ring, tuş raporu çözme ve mevcut input event queue’ye aktarma yoktur. xHCI **controller driver’ının** PCI aygıtına bind olması farklı ve başarılı bir durumdur.

QEMU’nun ham seri kanıtı [`docs/usb-ddc-qemu-smoke.log`](docs/usb-ddc-qemu-smoke.log) içinde saklanır. Kaynakta tekrar çalıştırılabilir `tests/qemu_usb_smoke.py` ve `make test-usb-qemu` hedefi vardır. Build klasörü bir arşive dahil edilmezse test tekrar çalıştırılabilir. Tasarım ayrıntısı `docs/device-manager.md` ve `docs/usb-xhci-design.md` içindedir.

## 5. Son test durumu ve yeniden üretme

2026-10-14’teki son kayıtta:

- Clean `make -j2 all`: başarılı.
- `make test-device-manager`: **12 test geçti**.
- `make check`: başarılı.
- `make test-elf-corpus`: **38 vaka, 0 hata**.
- `make test-usb-qemu`: başarılı; seri assertion’ları CPU sayısı + DDC + USB interface/model.
- Initramfs CPIO içinde root entry `ddc.json` doğrulandı.

Bunlar geçmiş koşunun kanıtıdır; yeni çalışma ortamında, kaynak değiştiyse yeniden çalıştır:

```sh
make clean
make -j2 all
make check
make test-elf-corpus
make test-device-manager
make test-usb-qemu
```

BIOS/UEFI smoke için `make run` ve `make run-uefi` kullanılabilir. UEFI firmware dosya yolu makineye bağlıdır (`OVMF_CODE` Makefile değişkeni). USB smoke scripti QEMU’nun `qemu-xhci` ve `usb-kbd` cihazlarını desteklemesini ister.

Önemli kanıt kanalları: QEMU `-serial file:...` veya `-serial stdio`; boot başarı göstergesi `NPKernel initialization complete; interrupts enabled`. `-smp 2` AP-online kanıtı **tam SMP scheduling kanıtı değildir**.

## 6. Pratik kalan işler ve önerilen sıra

Bu sıralama kullanıcının “ilk sürümde kusursuzluk değil, gerçekten zorunlu kernel işleri; sonra güvenlik düzeltmeleri ve yeni driver/özellikler” amacına göredir. Her maddeyi release kapsamına göre açıkça etiketle; her şeyi v0.1/v0.4 bloklayıcısı ilan etme.

### A. Kullanıcının önceki taleplerinden tamamlanan kullanıcı-facing işler

1. **USB HID boot keyboard gerçek input’u:** bounded boot-protocol USB HID klavye yolu DDC `usb_hid_keyboard` driver bind’i, xHCI interrupt-IN raporları ve QEMU key injection ile doğrulandı. Kanıt ve kapsam sınırları `docs/verification.md` ile `docs/usb-xhci-design.md` içindedir; mouse benzer aşaması ayrıca ele alınır.
2. **TTY stdin ve userspace shell ELF:** FD 0 satır tabanlı blocking read/wakeup kullanır; varsayılan boot 1024×768 siyah GOP yüzeyinde VFS `/bin/bash.elf` dosyasını ring 3’te başlatır. `user/bash.c`, freestanding mini-libc ve QEMU USB komut enjeksiyon smoke testi vardır. `exec` ELF’i mevcut süreçte başlatıp shell’i değiştirir; fork/wait ile child komutu yürütme henüz ek iş. Bu GNU Bash veya tam Linux userspace değildir; desteklenen komut/syscall alt kümesi belgelenmiştir. Exit sonrası respawn/session yönetimi ayrıca ele alınır.

### B. İlk kullanılabilir sürüm için release gate’leri

- Kesin release kapsamı/iddiası: BIOS/UEFI boot, desteklenen VFS/syscall subset’i, security sınırları, tek-BSP scheduler ve USB diagnostic vs USB input ayrımı.
- Temiz üretim build’i, `make check`, adversarial ELF corpus; BIOS + UEFI QEMU seri boot; persist/NPKFS testleri; `-smp 2` AP bring-up testi; USB testini iddia edilen düzeyde çalıştır.
- Log/kanıtları source docs altında tarihle ve konfigürasyonu belirt; varsayılan shell etkinliği ile `NPK_ENABLE_USER_DEMO=0` ayrımını koru.
- Güvenlik/robustness audit’i mevcut kod ve gerçek user-originated fault/syscall testleriyle tazele. Önceki README/verification/audit’teki bounded sınırlarıyla kaynak davranışını karşılaştır. Bilinmeyen syscalls’ın ENOSYS verdiğini koru.
- AP’lerin online ama idle olduğunu release notes’ta açıkla; tam per-CPU SMP scheduler tamamlanmadan paralel userspace veya bütün SMP safety varmış gibi yazma. Tek-BSP erken sürüm hedefi kabul edilirse, bunu kalan açık özellik değil belirtilmiş kapsam diye ele al.

### C. Bilinçli olarak sonraya bırakılabilecek işler

Tam per-CPU SMP scheduler/IPI/timer routing/migration; gerçek glibc/musl static runtime testi ve userspace dynamic relocation; tüm pthread/Rust runtime semantiği; realtime sigwait/nested frame’ler; blocking pipe/select/edge-triggered epoll; persistent directory tree, data journaling ve power-failure data integrity; PCI resource allocation/MSI/DMA/IOMMU; USB hub/hotplug, network, audio, AHCI/NVMe; tam Linux pathname/permission namespace. Bunları ancak kullanıcı kapsamına ya da release iddiasına girerse öne çek.

## 7. Önemli dosyalar

| Amaç | Dosya |
|---|---|
| İlk okuma / bu devir | `agent.md` |
| Ürün hedefi ve kapsam | `README.md` |
| Açık syscall/kernel işleri | `TODO.md` |
| Release/aşama planı | `docs/kernel-completion-plan.md` |
| Test kanıtı | `docs/verification.md`, `docs/final-kernel-gates.log` |
| Güncel bounded audit | `docs/v0.3-audit.md` |
| Userspace sözleşmesi | `docs/user-abi.md`, `include/npk/user_abi.h` |
| Device manager/DDC | `include/npk/device_manager.h`, `include/npk/device_config.h`, `src/device/device_manager.c`, `src/device/device_config.c`, `user/ddc.json` |
| USB/xHCI | `include/npk/usb.h`, `src/drivers/usb.c`, `src/drivers/xhci.c`, `docs/usb-xhci-design.md` |
| QEMU/host regression | `tests/device_manager_host.c`, `tests/qemu_usb_smoke.py`, `tests/elf_validate_host.c` |
| Boot sırası | `src/kernel.c` |
