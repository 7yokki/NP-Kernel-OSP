# BSP idle dönüşü ve ELF `exit` doğrulaması

## Bulgulanan hata

Kullanıcı ELF’i `exit(60)` veya `exit_group(231)` çağırdığında syscall yolu thread/process’i sonlandırıp `scheduler_yield()` çağırıyordu. Fakat zamanlayıcı yalnızca `THREAD_READY` thread’leri seçiyordu; `threads[0]` kernel ana thread’i `THREAD_RUNNING` olarak kalıyor ve scheduler tarafından devam hedefi olarak kullanılmıyordu. Başka hazır thread yoksa syscall yolundaki son `arch_halt()` kalıcı beklemeye neden oluyordu. Bu, tek başına AP/SMP bring-up hatası değildi.

AP’ler mevcut tasarımda ayrı mimari durum kurulduktan sonra bilinçli biçimde `cli; hlt` ile quiescent bekletilir. Per-CPU scheduler, AP interrupt/IPI yönlendirmesi ve migration hâlâ kapsam dışıdır. Kullanıcı ELF çıkışından sonraki BSP beklemesi ise ayrıca gerçek bir idle hedefi bulunmamasından kaynaklanıyordu.

## Düzeltme

- `process_init()` artık BSP için kendi kernel stack’i olan gerçek idle thread oluşturuyor; oluşturma başarısız olursa kernel erken panic ediyor.
- Zamanlayıcı olağan kullanıcı thread’lerini idle thread’e tercih ediyor.
- Timer preemption, tek seçenek idle olduğunda çalışan kullanıcı işini gereksiz yere idle’a atlamıyor. ELF çıkışında ise scheduler idle thread’i seçip BSP’yi `sti; hlt` döngüsüne geçiriyor.
- Kernel idle’dan yeniden ring-3’e geçişte `SWAPGS` kararı eski thread’in değil hedef thread’in privilege seviyesine göre veriliyor.
- `USER_PAYLOAD=c-exit` ve `NPK_ENABLE_USER_DEMO=1` test seçeneği eklendi. Varsayılan assembly payload ve demo-kapalı build davranışı değişmedi.

## Örnek C ELF

`tests/user_exit.c` freestanding x86_64 ELF’tir. `write(1, "C_ELF_EXIT_TEST...", ...)` çağrısı yapar; yazma uzunluğu doğruysa `exit(0)`, değilse `exit(1)` ile çıkar.

Yeniden build:

```sh
make clean
make -j2 all USER_PAYLOAD=c-exit NPK_ENABLE_USER_DEMO=1
```

İki vCPU ile çalıştırma:

```sh
qemu-system-x86_64 -M q35 -smp 2 -m 512M \
  -cdrom build/npkernel.iso \
  -serial file:build/qemu-c-elf-smp2.serial.log \
  -no-reboot -no-shutdown
```

Beklenen seri sıra: iki CPU online, syscall 1 (`write`), syscall 60 (`exit`), ardından `sched: BSP idle thread entered after user workload`. Grafik konsolda C programının yazdırdığı satır görünür.

## Doğrulama kaydı

2026-10-13 tarihli QEMU Q35/SMP=2 smoke testi:

- Limine MP: 2 CPU keşfedildi; AP bring-up istendi: 1; online: 2.
- Kernel ELF: ELF64 EXEC, x86-64; C test ELF’i `0x400000` girişli, salt-okunur/yürütülebilir PT_LOAD segmentleri içeriyor.
- Kullanıcı testinde syscall `1` ve `60` görüldü; sonrasında BSP idle thread giriş işareti seri loga yazıldı.
- Seri logda `PANIC`, `#GP` veya fatal page-fault işareti yok.
- QEMU ekran görüntüsü C satırını, `exit` syscall’ını ve idle thread’e geçişi gösteriyor.
- `make check test-elf-corpus USER_PAYLOAD=c-exit NPK_ENABLE_USER_DEMO=1`: başarılı; ELF corpus 38 vaka, 0 başarısızlık.

Bu test AP bring-up’ın iki CPU online olduğunu doğrular; SMP scheduler çalıştırıldığını veya AP’lerin kullanıcı kodunu yürüttüğünü iddia etmez.
