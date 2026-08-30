# Güvenli userspace tasarımı için teknik bulgular

## SYSRET

Intel x86-64 SYSRET referansı, dönüş RIP’inin RCX’ten ve RFLAGS’in R11’den yüklendiğini; RSP’nin SYSRET tarafından değiştirilmediğini belirtir. Bu nedenle kernel, kullanıcı stack’ine dönmeden önce doğru RSP’yi kurmalı ve o aralıkta maskelenebilir interrupt’ların kullanıcı stack’i üzerinde kernel handler’a girmesini engellemelidir. RCX canonical değilse SYSRET `#GP` üretebilir; dönüşten önce canonical adres kontrolü ve #GP için güvenilir IST/stack yolu gerekir. SYSRET’in CS/SS descriptor cache’lerini normal GDT descriptor’larından yüklemediği, STAR alanından türetilen selector’lerle sabit cache değerleri kullandığı da dikkate alınmalıdır.

Kaynak: [Intel/AMD instruction reference — SYSRET](https://www.felixcloutier.com/x86/sysret)

## ELF PT_LOAD

System V ABI program header tanımına göre `p_filesz <= p_memsz` olmalıdır. Dosyadaki bytes segment belleğinin başlangıcına yüklenir; `p_memsz > p_filesz` farkı sıfırla başlatılır. PT_LOAD girişleri `p_vaddr` artan sırada gelmelidir. `p_align` 0 veya 1 değilse pozitif iki kuvveti olmalı ve `p_vaddr` ile `p_offset`, `p_align` modulo eşdeğer olmalıdır. NPKernel loader bu koşulları, overflow, canonical user range, segment overlap ve page-permission policy ile birlikte kontrol etmelidir.

Kaynak: [System V ABI — Program Header](https://refspecs.linuxbase.org/elf/gabi4+/ch5.pheader.html)

## Tasarım sonucu

Bu belge ilk tasarım kararının tarihsel kaydıdır. Güvenli userspace geçişi için `IRETQ` tabanlı dönüş yolu korunur; SYSRET ancak RCX/R11/RSP ve GDT/STAR koşulları açıkça doğrulanıp #GP/NMI stack yolları kurulduğunda kullanılabilir. Güncel loader, ELF `ET_DYN` için bounded load bias kullanır ve `PT_INTERP` metadata’sını doğrulayıp interpreter image’ını aynı address space’e map eder. Bu, kernel dynamic-linker handoff’udur: relocation, `DT_NEEDED` ve sembol çözümleme userspace `ld.so` sorumluluğunda kaldığı için tam glibc/musl dinamik binary uyumluluğu iddia edilmez.
