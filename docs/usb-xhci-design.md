# USB HID boot keyboard ve xHCI tasarım notu

## Uygulama akışı

1. `usb_init()` önce `usb_generic_keyboard` DDC driver’ını, ardından PCI xHCI driver’ını kaydeder.
2. xHCI enumerasyonu root-port reset, Enable Slot, Address Device ve EP0 standard control transferlarıyla yürür; USB core device descriptor ve bounded configuration descriptor’ı alır.
3. USB core yalnızca tek interface/tek interrupt-IN endpoint içeren alternate-setting-0 HID Boot Keyboard subset’ini seçer. Interface, HID class descriptor ve Report descriptor uzunluğu ile endpoint MPS/interval alanları sınırları kontrol edilerek kaydedilir.
4. USB core transfer ring ve Input Context’i hazırlar; xHCI `Configure Endpoint` komutunu başarıyla tamamlar. Ardından USB `SET_CONFIGURATION` isteği gönderilir. Bu sıra Intel xHCI 1.2 §4.3.5 ve §4.6.6 ile uyumludur.
5. DDC eşleşmesi `usb_generic_keyboard_probe()` çağırır. Driver `GET_DESCRIPTOR(Report)` ile rapor tanımlayıcısını alır ve Generic Desktop/Keyboard Application Collection olduğunu kontrol eder; `SET_PROTOCOL(Boot)` ile HID boot protokolünü seçer.
6. xHCI interrupt-IN endpoint transfer ring’ine 8-byte Normal TRB bırakır. Kernel ana döngüsü event ring’i poll eder; tamamlanan rapor önceki raporla karşılaştırılır ve değişen modifier/key usage değerleri mevcut ortak kernel keyboard event/text queue’suna aktarılır.

## xHCI ring ve DMA ayrıntıları

Control ve interrupt ring’leri, endpoint context içindeki TR Dequeue Pointer/DCS ve üretici cycle state’iyle yönetilir. Link TRB’ye yazılırken Link TRB’nin Cycle bit’i **mevcut segment cycle’ı** olarak bırakılır; Link TRB’nin Toggle Cycle biti tüketici tarafında bir sonraki segmente geçerken cycle’ı değiştirir. Producer’ın cycle alanı bu geçişten sonra güncellenir. İlk gerçek QEMU denemesinde Link TRB cycle’ının producer tarafından bir segment erken değiştirilmesi ring wrap sonrasında Report descriptor control transfer’ını beklemeye soktu. Kontrol, command ve keyboard transfer ring’leri bu sıraya göre düzeltildi.

FS/LS interrupt bInterval için xHCI Endpoint Context interval alanı Intel tablosundaki `bInterval * 8` microframe hesabının aşağı yuvarlanmış 2 tabanlı üssüne çevrilir (alan aralığı 3–10). HS bInterval 1–16 için context interval `bInterval-1` olur. Diğer hızlar şu an bu keyboard yolu tarafından kabul edilmez. MPS’in high-bandwidth/reserved üst bitleri maskelenip yanlış yorumlanmaz; aday keyboard endpoint’i bu encoding’i kullanıyorsa reddedilir.

DMA ring/context/report buffer’ları hizalı kernel page allocator sayfalarında tutulur. Interrupt transferı her seferinde tek 8-byte rapor ister; completion sonrası report driver’a aktarılır ve sonraki Normal TRB requeue edilir. PS/2 `keyboard.c` handler ve legacy path kaldırılmamış/değiştirilmemiştir; HID için ek event adapter aynı input queue’yu kullanır.

## Boot report ve klavye event’leri

HID 1.11 Boot Keyboard raporu 8 byte’tır: modifier bitleri, implementation-specific ikinci byte, ardından en çok altı keyboard usage alanı. İkinci byte HID 1.11 Appendix F.4 uyarınca yok sayılır. Key sırası anlamlı olmadığından driver usage set’lerini önceki raporla karşılaştırır; `ErrorRollOver` (usage 1) halinde eski key array’ini bırakıp sahte key-release üretmez. Modifier değişimleri ayrı event’ler olarak iletilir. Mapping HID Usage Tables Keyboard/Keypad Page (0x07) temel harf/rakam/noktalama, function, navigation, keypad ve modifier usage’larını mevcut PS/2 tarzı input keycode/text yoluna eşler.

## Kapsam ve sınırlar

- Yalnızca ilk configuration, tek alternate-setting-0 interface, HID class/subclass/protocol `03/01/01` ve tek interrupt-IN endpoint desteklenir.
- HID Report descriptor için sınırlı item decoder yalnızca Keyboard Application Collection’ı doğrular; genel report layout’larını decode etmez. Input yolu boot protocol’dür.
- USB SuperSpeed/endpoint companion descriptor, birden çok endpoint/configuration/interface, hub/topology, hotplug/remove, birden çok controller, interrupt/MSI, mass storage/BOT, DMA/IOMMU izolasyonu ve fiziksel donanım testi bu değişikliğin kapsamı dışındadır.
- PS/2 sürücüsü geriye dönüklülük için korunur. xHCI/device registry halen boot-time serialized akıştadır; bu uygulama IRQ/SMP-safe hotplug desteği sağlamaz.

## Doğrulama

- `make test-usb-keyboard`: report diff, modifier, rollover/recovery, short report reddi ve ikinci byte yok sayma testleri.
- `make test-usb-qemu`: QEMU `qemu-xhci` + `usb-kbd` ile boot; QMP `send-key` komutuyla `a` basılı tutulup bırakılır; kernel serial log HID usage `0x04` press/release alındığını doğrular.
- Tam üretim build/ELF corpus/Device Manager suite’leri için `docs/verification.md` içindeki “USB HID boot keyboard” kaydına bakın.

## Kaynaklar

- [USB-IF HID 1.11](https://www.usb.org/sites/default/files/documents/hid1_11.pdf): §7.2.6 Set_Protocol; Appendix B boot descriptors; Appendix F.3 boot keyboard requirements, F.4 host-side report comparison/order and second-byte rule.
- [USB-IF HID Usage Tables 1.12](https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf): §10, Keyboard/Keypad Page (0x07), Table 12.
- [Intel xHCI 1.2 Requirements Specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf): §4.3.5 Device Configuration; §4.6.6 Configure Endpoint; §6.2.3.6 interval translation/Table 6-12; §4.9 ring/TRB cycle semantics.
- [QEMU QMP Reference Manual](https://qemu-project.gitlab.io/qemu/interop/qemu-qmp-ref.html): `send-key` `KeyValue` qcode/hold-time contract.
- [QEMU USB emulation](https://qemu-project.gitlab.io/qemu/system/devices/usb.html): qemu-xhci and USB device configuration context.
