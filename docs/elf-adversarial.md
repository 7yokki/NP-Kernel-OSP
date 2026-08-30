# ELF adversarial corpus

NPKernel’in `elf64_validate()` fonksiyonu için host üzerinde çalıştırılan, tekrar üretilebilir bir adversarial corpus vardır. Test, loader’ın gerçek validation kodunu freestanding olmayan bir host harness ile çağırır; bu nedenle yalnızca Python tarafında tekrar edilen bir politika testi değildir.

## Çalıştırma

```sh
make test-elf-corpus
```

Bu hedef `build/user/hello.elf` üzerinden `build/adversarial-elf/` altında vakaları üretir, `src/exec/elf.c` içindeki `elf64_validate()` fonksiyonunu host harness ile derler ve manifestteki beklenen boolean sonuçlarla gerçek sonuçları karşılaştırır.

## Mevcut corpus

| Kategori | Vakalar |
|---|---|
| Geçerli image’lar | `valid`, `interp_valid`, `dynamic_valid`, `et_dyn_valid` |
| Header kimliği ve ABI | `truncated_header`, `bad_magic`, `bad_class`, `bad_data`, `bad_version`, `bad_machine` |
| Program-header tablo sınırları | `phoff_wrap`, `zero_phnum`, `short_phentsize`, `phnum_over_limit`, `interp_duplicate`, `dynamic_duplicate` |
| Segment boyutu ve dosya sınırları | `filesz_gt_memsz`, `zero_memsz`, `file_offset_oob`, `offset_filesz_wrap`, `excessive_memsz`, `interp_filesz_gt_memsz`, `dynamic_filesz_gt_memsz` |
| Alignment ve izinler | `bad_alignment`, `vaddr_offset_misaligned`, `writable_executable` |
| User-range ve giriş noktası | `non_user_vaddr`, `entry_nonexec`, `entry_bias_wrap` |
| Segment çakışması | `overlapping_loads` |
| Dynamic metadata | `dynamic_unterminated`, `dynamic_bad_size`, `dynamic_negative_tag`, `dynamic_filesz_gt_memsz`, `dynamic_duplicate` |
| Interpreter path | `interp_unterminated`, `interp_relative`, `interp_embedded_nul`, `interp_path_traversal`, `interp_too_long` |

Son doğrulama kapısında **38 vaka / 0 başarısızlık** elde edilmiştir. Geçerli dört image kabul edilmiş, bozuk veya politika dışı 34 image reddedilmiştir. Testin kapsamı page-table rollback leak’lerini ölçen QEMU fault-injection testi değildir; loader’ın kaynak rollback kapısı ayrıca kaynak ve QEMU testleriyle korunur.

## Güvenlik politikası

`PT_INTERP` yolu yalnızca NUL-terminated, bounded, printable, absolute ve `..` component içermeyen bir path olarak kabul edilir; `filesz > memsz` metadata da reddedilir. `PT_DYNAMIC` tablosu image sınırları içinde olmalı, `filesz <= memsz`, entry-size ile hizalı olmalı, `DT_NULL` ile bitmeli ve negatif tag içermemelidir. Kernel interpreter’ın PT_LOAD segmentlerini map eder; sembol çözümleme, `DT_NEEDED` grafiği ve relocation policy’si userspace `ld.so` sorumluluğudur.
