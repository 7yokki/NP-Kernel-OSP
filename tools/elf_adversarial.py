#!/usr/bin/env python3
"""Generate bounded malformed ELF images for the kernel validator harness."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EHDR = struct.Struct("<16sHHIQQQIHHHHHH")
PHDR = struct.Struct("<IIQQQQQQ")
PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PF_X = 1
PF_W = 2
PF_R = 4


def patch_header(image: bytearray, **changes: int) -> None:
    fields = list(EHDR.unpack_from(image, 0))
    names = ["ident", "type", "machine", "version", "entry", "phoff", "shoff", "flags",
             "ehsize", "phentsize", "phnum", "shentsize", "shnum", "shstrndx"]
    for name, value in changes.items():
        fields[names.index(name)] = value
    EHDR.pack_into(image, 0, *fields)


def get_phdr(image: bytearray, index: int = 0) -> tuple[int, ...]:
    _, _, _, _, _, phoff, _, _, _, phentsize, phnum, _, _, _ = EHDR.unpack_from(image, 0)
    if index >= phnum or phentsize < PHDR.size:
        raise ValueError("program header index out of range")
    return PHDR.unpack_from(image, phoff + index * phentsize)


def put_phdr(image: bytearray, index: int, values: tuple[int, ...]) -> None:
    _, _, _, _, _, phoff, _, _, _, phentsize, phnum, _, _, _ = EHDR.unpack_from(image, 0)
    if index >= phnum or phentsize < PHDR.size:
        raise ValueError("program header index out of range")
    PHDR.pack_into(image, phoff + index * phentsize, *values)


def mutate_phdr(image: bytearray, index: int = 0, **changes: int) -> bytearray:
    values = list(get_phdr(image, index))
    names = ["type", "flags", "offset", "vaddr", "paddr", "filesz", "memsz", "align"]
    for name, value in changes.items():
        values[names.index(name)] = value
    put_phdr(image, index, tuple(values))
    return image


def synthetic_image(*, interp: bytes | None = None, dynamic: bytes | None = None) -> bytes:
    phnum = 1 + (1 if interp is not None else 0) + (1 if dynamic is not None else 0)
    phoff = EHDR.size
    data_offset = 0x1000
    payload = bytearray(data_offset + 0x400)
    ident = bytearray(b"\x7fELF") + bytearray([2, 1, 1]) + bytearray(9)
    EHDR.pack_into(payload, 0, bytes(ident), 2, 62, 1, 0x400000, phoff, 0, 0,
                   EHDR.size, PHDR.size, phnum, 0, 0, 0)
    PHDR.pack_into(payload, phoff, PT_LOAD, PF_R | PF_X, data_offset, 0x400000, 0,
                   1, 1, 0x1000)
    payload[data_offset] = 0xC3
    index = 1
    cursor = 0x1100
    if interp is not None:
        PHDR.pack_into(payload, phoff + index * PHDR.size, PT_INTERP, 0, cursor, 0, 0,
                       len(interp), len(interp), 1)
        payload[cursor:cursor + len(interp)] = interp
        index += 1
        cursor += 0x100
    if dynamic is not None:
        PHDR.pack_into(payload, phoff + index * PHDR.size, PT_DYNAMIC, 0, cursor, 0, 0,
                       len(dynamic), len(dynamic), 8)
        payload[cursor:cursor + len(dynamic)] = dynamic
    return bytes(payload[:max(data_offset + 1, cursor + 0x100)])


def main() -> int:
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build/user/hello.elf")
    output = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("build/adversarial-elf")
    if not source.is_file():
        raise SystemExit(f"source ELF not found: {source}")
    output.mkdir(parents=True, exist_ok=True)
    for child in output.iterdir():
        if child.is_file():
            child.unlink()
    base = bytearray(source.read_bytes())
    cases: list[tuple[str, bytes, int]] = [("valid", bytes(base), 1)]

    def add(name: str, image: bytearray | bytes, expected: int = 0) -> None:
        cases.append((name, bytes(image), expected))

    add("truncated_header", base[:32])
    image = bytearray(base); image[0] = 0; add("bad_magic", image)
    image = bytearray(base); image[4] = 1; add("bad_class", image)
    image = bytearray(base); image[5] = 2; add("bad_data", image)
    image = bytearray(base); patch_header(image, machine=3); add("bad_machine", image)
    image = bytearray(base); patch_header(image, phoff=(1 << 64) - 1); add("phoff_wrap", image)
    image = bytearray(base); patch_header(image, phnum=0); add("zero_phnum", image)
    image = bytearray(base); patch_header(image, phentsize=1); add("short_phentsize", image)
    image = bytearray(base); mutate_phdr(image, filesz=get_phdr(image)[6] + 1, memsz=get_phdr(image)[6]); add("filesz_gt_memsz", image)
    image = bytearray(base); mutate_phdr(image, offset=(1 << 63)); add("file_offset_oob", image)
    image = bytearray(base); mutate_phdr(image, align=3); add("bad_alignment", image)
    image = bytearray(base); mutate_phdr(image, flags=PF_R | PF_W | PF_X); add("writable_executable", image)
    image = bytearray(base); mutate_phdr(image, vaddr=0); add("non_user_vaddr", image)
    image = bytearray(base); patch_header(image, entry=0); add("entry_nonexec", image)
    image = bytearray(base); patch_header(image, version=0); add("bad_version", image)
    image = bytearray(base); patch_header(image, phnum=65); add("phnum_over_limit", image)
    image = bytearray(base); mutate_phdr(image, memsz=0); add("zero_memsz", image)
    image = bytearray(base); mutate_phdr(image, offset=len(image) - 1, filesz=2); add("offset_filesz_wrap", image)
    image = bytearray(base); mutate_phdr(image, vaddr=0x400001); add("vaddr_offset_misaligned", image)
    image = bytearray(base); mutate_phdr(image, memsz=0x100000000); add("excessive_memsz", image)
    image = bytearray(base); patch_header(image, entry=(1 << 64) - 1); add("entry_bias_wrap", image)

    _, _, _, _, _, phoff, _, _, _, phentsize, phnum, _, _, _ = EHDR.unpack_from(base, 0)
    if phnum > 1:
        image = bytearray(base)
        first = list(PHDR.unpack_from(image, phoff))
        second_offset = phoff + phentsize
        PHDR.pack_into(image, second_offset, PT_LOAD, first[1], first[2], first[3], first[4], first[5], first[6], first[7])
        add("overlapping_loads", image)

    interp_image = bytearray(synthetic_image(interp=b"/lib64/ld-npk.so\0"))
    add("interp_valid", interp_image, 1)
    image = bytearray(interp_image); mutate_phdr(image, 1, memsz=get_phdr(image, 1)[5] - 1); add("interp_filesz_gt_memsz", image)
    image = bytearray(interp_image); patch_header(image, phnum=3); PHDR.pack_into(image, EHDR.size + 2 * PHDR.size, *get_phdr(image, 1)); add("interp_duplicate", image)
    add("interp_unterminated", synthetic_image(interp=b"/lib64/ld-npk.so"))
    add("interp_relative", synthetic_image(interp=b"ld-npk.so\0"))
    add("interp_embedded_nul", synthetic_image(interp=b"/lib64/ld\0.so\0"))
    add("interp_path_traversal", synthetic_image(interp=b"/lib/../ld-npk.so\0"))
    add("interp_too_long", synthetic_image(interp=b"/" + b"a" * 256 + b"\0"))

    dynamic_image = bytearray(synthetic_image(dynamic=struct.pack("<qQ", 1, 0) + struct.pack("<qQ", 0, 0)))
    add("dynamic_valid", dynamic_image, 1)
    image = bytearray(dynamic_image); mutate_phdr(image, 1, memsz=get_phdr(image, 1)[5] - 1); add("dynamic_filesz_gt_memsz", image)
    image = bytearray(dynamic_image); patch_header(image, phnum=3); PHDR.pack_into(image, EHDR.size + 2 * PHDR.size, *get_phdr(image, 1)); add("dynamic_duplicate", image)
    add("dynamic_unterminated", synthetic_image(dynamic=struct.pack("<qQ", 1, 0)))
    add("dynamic_bad_size", synthetic_image(dynamic=b"\\x01" * 8))
    add("dynamic_negative_tag", synthetic_image(dynamic=struct.pack("<qQ", -1, 0) + struct.pack("<qQ", 0, 0)))
    image = bytearray(synthetic_image())
    patch_header(image, type=3)
    add("et_dyn_valid", image, 1)

    manifest = output / "manifest.tsv"
    with manifest.open("w", encoding="utf-8") as listing:
        for name, content, expected in cases:
            path = output / f"{name}.elf"
            path.write_bytes(content)
            listing.write(f"{path}\t{expected}\n")
    print(f"generated {len(cases)} ELF cases in {output}")
    print(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

