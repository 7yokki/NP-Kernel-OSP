from pathlib import Path
import sys

PROJECT_ROOT = Path(__file__).resolve().parent.parent
ROOT = PROJECT_ROOT / 'initramfs'
OUTPUT = Path(sys.argv[1]) if len(sys.argv) > 1 else PROJECT_ROOT / 'build' / 'initramfs.cpio'


def field(value: int) -> bytes:
    return f'{value:08x}'.encode('ascii')


def record(name: str, data: bytes, mode: int) -> bytes:
    name_bytes = name.encode('utf-8') + b'\0'
    header = b''.join([
        b'070701',
        field(0),
        field(mode),
        field(0),
        field(0),
        field(1),
        field(0),
        field(len(data)),
        field(0),
        field(0),
        field(0),
        field(0),
        field(len(name_bytes)),
        field(0),
    ])
    result = header + name_bytes
    result += b'\0' * ((-len(result)) % 4)
    result += data
    result += b'\0' * ((-len(result)) % 4)
    return result


OUTPUT.parent.mkdir(parents=True, exist_ok=True)
archive = bytearray()
for path in sorted(ROOT.rglob('*')):
    if path.is_file():
        relative = path.relative_to(ROOT).as_posix()
        archive += record(relative, path.read_bytes(), 0o100644)
for name in ('hello', 'mmap', 'fork'):
    user_elf = OUTPUT.parent / 'user' / f'{name}.elf'
    if user_elf.is_file():
        archive += record(f'bin/{name}.elf', user_elf.read_bytes(), 0o100755)
archive += record('TRAILER!!!', b'', 0)
OUTPUT.write_bytes(archive)
print(f'{OUTPUT}: {len(archive)} bytes')
