# Device Manager ve Default Device Catalog (DDC)

## Kimlik katmanları

USB’de **kategori** ve **ürün/model** ayrı çözülür:

- **Kategori:** configuration içindeki interface descriptor’ın `class/subclass/protocol` üçlüsü. Örneğin HID boot keyboard `03/01/01` → `keyboard`.
- **Ürün/model:** device descriptor’daki `VID:PID`. Katalogda eşleşirse modele özel ad; eşleşmezse kategorinin `generic_name` değeri kullanılır.

Bu nedenle bir üreticinin yeni bir klavyesi, VID/PID tablosunda bulunmasa bile keyboard kategorisinde kalır ve `Generic Keyboard` adıyla aynı sınıf driver’ına yönelir.

## Katalog yolu ve VFS

Kaynak dosya `user/ddc.json` konumundadır. `tools/make_initramfs.py` bunu initramfs root’una `ddc.json` olarak ekler; çalışma anında dosya VFS üzerinden **`/ddc.json`** yolundadır ve initramfs içeriği değiştirilemez/read-only’dir. Kernel sırası şöyledir:

1. `vfs_init()` initramfs’i mount eder.
2. `devmgr_load_default_config()` Device Manager API’si üzerinden `vfs_open("/ddc.json")`, `vfs_size()`, `vfs_read()` ve `vfs_close()` kullanır.
3. `usb_init()` önce `usb_generic_keyboard` driver’ını, ardından xHCI PCI driver’ını kaydeder; xHCI probe bu sırada USB enumerasyonunu başlatır.

Device manager veya USB code initramfs’in ham adresini/path’ini doğrudan okumaz. JSON parser, VFS’den alınan boyutu sınırlı içeriği ayrıştırır; ham dosya erişim yöntemi yalnızca VFS’dir.

## DDC v1 biçimi

```json
{
  "version": 1,
  "categories": [
    {
      "bus": "usb",
      "class": 3,
      "subclass": 1,
      "protocol": 1,
      "type": "keyboard",
      "generic_name": "Generic Keyboard",
      "driver": "usb_hid_keyboard"
    }
  ],
  "products": [
    {
      "vid": 1575,
      "pid": 1,
      "type": "keyboard",
      "name": "QEMU USB Keyboard"
    }
  ]
}
```

`class/subclass/protocol` kategori anahtarıdır. `products[]` yalnızca model adını çözer; ürün kaydının bulunmaması kategori eşleşmesini düşürmez. `driver` alanı kategoriye ait driver’ın kayıt adıdır.

Parser v1’de dosya 16 KiB, kategori sayısı 16, ürün/model sayısı 64 ile sınırlıdır; USB kategorileri tam class tuple ile eşleşir. Geçersiz/eksik katalog yüklenmez; USB tanılaması devam eder fakat interface’ler sınıflandırılmadan kalır.

## Enumerasyon ve dispatcher

xHCI, device descriptor’ın ilk 8 byte’ını okuyup EP0 packet size’ı ayarlar, tam 18-byte device descriptor’ı alır, ardından configuration descriptor’ı (header ve bounded full descriptor) okur. USB core her configuration’daki **alternate setting 0** interface descriptor’ı için VID/PID + interface class tuple içeren ayrı manager kaydı açar. Device descriptor’ın class alanı `0` ise bu normaldir; gerçek kategori interface’te bulunabilir.

Device Manager önce interface tuple’ını DDC category ile eşler; ürün VID/PID’si varsa display name’i model adıyla değiştirir, yoksa generic name bırakır. Driver kaydı varsa ID tablosu eşleşmesinden sonra DDC’de tanımlı driver adıyla aynı olan driver’a probe edilir. Kategori tanınsa da o isimde driver henüz register edilmemişse cihaz `UNBOUND` kalır ve beklenen driver loglanır. Böylece henüz yazılmamış bir HID driver’a sahte “bound/input hazır” sonucu verilmez.

## Şu anki kapsam ve sınırlar

- xHCI polling + QEMU `qemu-xhci` üzerinde device/configuration descriptor, interface kimliği ve HID boot-keyboard input yolu çalışır.
- `user/ddc.json`, QEMU `0627:0001` modelini `QEMU USB Keyboard` olarak adlandırır. Bilinmeyen VID/PID’li `03/01/01` interface için kategori adı `Generic Keyboard` olur.
- `usb_generic_keyboard` yalnızca bounded boot-keyboard subset’ini bind eder: ilk configuration, tek alternate-setting-0 HID boot keyboard interface, bir interrupt-IN endpoint, report-descriptor application check, `SET_PROTOCOL(Boot)`, 8-byte report diff ve ortak kernel key/text event queue. xHCI endpoint `Configure Endpoint` komutuyla etkinleştirilir ve bundan sonra USB `SET_CONFIGURATION` yollanır.
- `make test-usb-qemu` QEMU QMP üzerinden `a` tuşunu enjekte eder ve report usage `0x04` press/release olaylarının serial log’da kernel HID sürücüsüne ulaştığını doğrular; bu, gerçek fiziksel klavye veya gerçek donanım doğrulaması değildir.
- Destek sınırları: çok arayüzlü/alternatifli configuration seçimi, birden çok endpoint, HID report-protocol/değişken descriptor parser’ı, USB SuperSpeed endpoint companion, hub/topology, hotplug/remove, birden fazla controller, interrupt/MSI, USB storage/BOT, DMA/IOMMU izolasyonu ve gerçek donanım doğrulaması kapsam dışıdır.
- Manager şu aşamada boot-time BSP serialized registry’dir; IRQ-safe/SMP-safe değildir. Hotplug olduğunda discovery/probe worker’a ertelenmelidir.

## Testler

- `make test-device-manager`: VFS `/ddc.json` çağrısı, DDC parsing, exact VID/PID model adı, unknown product → `Generic Keyboard`, kategoriye driver adıyla dispatch, farklı protocol ve eski PCI manager davranışlarını host üzerinde test eder.
- `make test-usb-qemu`: ISO’yu QEMU q35, SMP=2, qemu-xhci ve usb-kbd ile boot eder; VFS catalog, iki online CPU, USB `0627:0001`, interface `03/01/01`, `keyboard` kategorisi, `QEMU USB Keyboard` adı, HID driver bind’i ve QMP ile enjekte edilen `a` press/release raporlarını bekler.
- `make test-usb-keyboard`: boot-report decoder’ında modifiers, key press/release, tekrar raporu, rollover koruması, rollover sonrası recovery, hatalı uzunluk ve implementation-specific ikinci bayt davranışını host’ta test eder.
