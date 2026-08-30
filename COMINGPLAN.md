# NP-Kernel Proje Yol Haritası ve Lisans Politikası

Bu doküman, NP-Kernel projesinin gelişim aşamalarını, organizasyon yapısını ve gelecek ticari dönüşüm planlarını içerir.

## 1. Gelecek Planı ve Stratejik Hedef
Çekirdek, genel amaçlı çekirdek ünvanını (temel vasfını) kazandığında projenin mevcut geliştirme ve dağıtım modeli tamamen değişecektir.

## 2. Kurumsal Yapı ve Yönetim
* Proje yürütücüsü: 7YokKi
* Çatı organizasyon: No-Problem
* Yönetim kararı: Çekirdek temel vasfını tamamladığında ticari modele geçiş yapılacaktır.

## 3. Gelişim, Dönüşüm ve Varyant Detayları
Projenin tam sürümü (v1) hazır olduğunda mevcut açık kaynaklı sürümün geliştirilmesi durdurulacaktır. Mevcut kodlar MIT Lisansı ile depoda kalmaya devam edecek, ancak eş zamanlı olarak "NP-FC Kernel" adıyla kapalı kaynak kodlu yeni bir ticari varyant oluşturulacaktır.

### 3.1 Gelişim Dönemi
* Proje v1 sürümüne kadar MIT Lisansı ile geliştirilmeye devam edecektir.
* Planlanan tüm çekirdek fonksiyonları bu süreçte tamamlanacaktır.

### 3.2 Sürücü Kısıtlamaları
* v1 sürümüne kadar donanımsal sürücü desteği dondurulacaktır.
* USB, Fare ve HID benzeri fiziksel donanım sürücüleri eklenmeyecektir.
* Sanal ve yazılımsal sürücüler bu kısıtlamanın dışındadır.

### 3.3 Ticari Dönüşüm (Devrim Aşaması)
* v1 sürümü, projenin son açık kaynaklı versiyonu olacaktır.
* Bu aşamadan sonra proje NP-FC Kernel adıyla kapalı kaynak olarak geliştirilecek ve ticari modele geçecektir.

### 3.4 NP-FC Kernel ve Büyük Proje Yapısı
NP-FC Kernel, "No Problem Source Secure v1" lisansı ile korunacaktır.

#### İzin Verilmeyen Faaliyetler
* Değiştirilmesi ve modifiye edilmiş halinin dağıtılması yasaktır.
* Tekrar yayınlanması yasaktır.
* Herhangi bir mecrada dağıtılması yasaktır.

#### İzin Verilen Faaliyetler
* Kod kopyalamamak şartıyla projeye dışarıdan destek olunabilir.
* Kaynak kod, tekrar paylaşılmaması şartıyla akademik olarak incelenebilir ve eleştirilebilir.
* Kod, tekrar paylaşılmaması şartıyla kişisel kullanım amacıyla modifiye edilebilir.
* Resmi izin alınması durumunda esnek koşullarla paylaşım yapılabilir.

## Bununla beraber

NPOS resmi olarak duyurulmasa da NP-FC kullanması ve Linux/Unix gibi sistemlerden daha kısıtlı olması beklenilmekte. Aynı zamanda Linux çatı desteğiyle ve bir çok güvenlik mekaniğiyle gelmesi bekleniyor.
