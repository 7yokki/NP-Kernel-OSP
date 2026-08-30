# NPKernel ACPI güç yönetimi doğrulaması

**Yazar:** Manus AI  
**Tarih:** 16 Ağustos 2026

## Kapsam

NPKernel’e firmware tarafından sağlanan ACPI keşfi ve sabit güç yönetimi desteği eklendi. Limine üzerinden alınan RSDP, ACPI 1.x sistemlerinde RSDT’ye; ACPI 2.0 ve sonrasında doğrulanmış XSDT’ye yönlendirilir. Kök tablo içinden FADT (`FACP`) ve DSDT keşfedilir. DSDT AML içindeki `_S5_` paketinden `SLP_TYP` değerleri çıkarılır ve FADT’nin PM1a/PM1b event-control blokları üzerinden güç düğmesi ile S5 soft-off uygulanır. Bu akış, ACPI sabit donanım kayıtlarının ve S5 yazılım kontrollü kapatmanın tanımına dayanır [1].

| Katman | Uygulanan davranış |
|---|---|
| RSDP | Limine API 0’da sanal pointer, diğer durumda HHDM üzerinden fiziksel adres olarak güvenli erişim |
| RSDT/XSDT | İmza, uzunluk ve checksum doğrulaması; ACPI 2+ için doğrulanmış XSDT tercihi |
| FADT | PM1a/PM1b event ve control blokları, GAS/X alanları, SCI IRQ ve ACPI enable komutu |
| DSDT AML | Sınır kontrollü `_S5_` NameOp/Package taraması; iki sleep type değeri desteklenir |
| Güç düğmesi | `PWRBTN_STS` temizlenir, `PWRBTN_EN` etkinleştirilir, SCI üzerinden işlenir |
| S5 | `SLP_TYP << 10` ile `SLP_EN` bitinin PM1 control yazımı; PM1b varsa ikinci yazım |
| SCI yönlendirmesi | MADT üzerinden IOAPIC ve interrupt-source override keşfi; aktif-düşük/seviye tetiklemeli vektör 41 kurulumu |
| Geri dönüş yolu | IOAPIC bulunamazsa legacy PIC IRQ9 ve periyodik PM1 status polling fallback’i |

## Güvenlik ve dayanıklılık sınırları

Tablo erişimleri sabit üst sınırlarla, imza/uzunluk/checksum kontrolleriyle ve HHDM adres taşması denetimiyle sınırlandırılmıştır. PM1 yazmaları yalnızca geçerli FADT kayıtları ve geçerli `_S5_` değerleri keşfedildiğinde gerçekleştirilir. ACPI enable geçişi, firmware’in SMI komutundan sonra sınırlı PM1 status polling ile beklenir; geri dönüşte sistem erişilebilir kalır ve başarısızlık loglanır.

Güncel sürüm tam AML yorumlayıcısı değildir. Yalnızca güvenli biçimde sınırlandırılmış, QEMU ve yaygın firmware’lerin kullandığı `_S5_` paket biçimi işlenir. Device tree, namespace çözümleme, GPE metodları ve genel AML yürütmesi sonraki ACPI kapsamındadır.

## QEMU doğrulaması

Aşağıdaki testlerde BIOS için `-M pc`, UEFI için OVMF ile `-M q35` kullanıldı. Sanal güç düğmesi QEMU QMP `system_powerdown` olayıyla üretildi; sonuç guest’in ACPI SCI/S5 yolundan gözlendi.

| Test | Gözlenen sonuç |
|---|---|
| SeaBIOS + RSDT + legacy IDE | `using RSDT`, `SCI IOAPIC GSI 9`, `ACPI PM1 power button, SCI, and S5 ready`, kernel initialization tamamlandı |
| OVMF UEFI + XSDT + Q35 | `using XSDT`, `SCI IOAPIC GSI 9`, `ACPI PM1 power button, SCI, and S5 ready` |
| OVMF UEFI power event | `power button event; entering S5 soft-off`; QMP `POWERDOWN` olayı alındı ve guest durdu |
| BIOS power path | PM1a event/control blokları ve S5 yolu başarıyla keşfedildi; legacy PIC/poll fallback’i korunuyor |

> **Sonuç:** QEMU’daki **Makine → Kapat** olayının kernel tarafından yok sayılmasına neden olan eksik ACPI katmanı giderildi. NPKernel artık firmware tablolarını kullanarak PM1 tabanlı güç düğmesi ve S5 soft-off yolunu işletiyor.

## Referanslar

[1]: https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/04_ACPI_Hardware_Specification/ACPI_Hardware_Specification.html "UEFI ACPI Specification 6.4 — ACPI Hardware Specification"
