#!/usr/bin/env python3
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


def fail(message, serial_path=None, process=None):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
    if serial_path and pathlib.Path(serial_path).exists():
        print(pathlib.Path(serial_path).read_text(errors='replace')[-12000:])
    raise SystemExit(message)


class QMP:
    def __init__(self, path):
        deadline = time.time() + 20
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        while True:
            try:
                self.sock.connect(path)
                break
            except OSError:
                if time.time() >= deadline:
                    raise
                time.sleep(0.1)
        self.file = self.sock.makefile('rwb', buffering=0)
        self._read_message()
        self.execute('qmp_capabilities')
        self.next_id = 1

    def _read_message(self):
        line = self.file.readline()
        if not line:
            raise RuntimeError('QMP connection closed')
        return json.loads(line)

    def execute(self, command, arguments=None):
        request_id = getattr(self, 'next_id', 0)
        self.next_id = request_id + 1
        request = {'execute': command, 'id': request_id}
        if arguments is not None:
            request['arguments'] = arguments
        self.file.write((json.dumps(request) + '\r\n').encode())
        while True:
            response = self._read_message()
            if response.get('id') != request_id:
                continue
            if 'error' in response:
                raise RuntimeError(f"QMP {command}: {response['error']}")
            return response.get('return')

    def close(self):
        try:
            self.file.close()
            self.sock.close()
        except OSError:
            pass


def keycode(character):
    if ('a' <= character <= 'z' and character != 'i') or '0' <= character <= '9':
        return character
    # QEMU QMP injects physical qcodes; model the XKB Turkish-Q key positions.
    mapping = {
        ' ': 'spc', '\n': 'ret', 'i': 'apostrophe',
        '*': 'minus', '?': 'shift+minus', '!': 'shift+1',
        "'": 'shift+2', '^': 'shift+3', '+': 'shift+4',
        '%': 'shift+5', '&': 'shift+6', '/': 'shift+7',
        '(': 'shift+8', ')': 'shift+9', '=': 'shift+0',
        '-': 'equal', '_': 'shift+equal', '.': 'slash',
    }
    if character in mapping:
        return mapping[character]
    raise ValueError(f'unsupported QMP test character {character!r}')


def send_command(qmp, command):
    for character in command:
        qcode = keycode(character)
        if '+' in qcode:
            keys = [{'type': 'qcode', 'data': part} for part in qcode.split('+')]
        else:
            keys = [{'type': 'qcode', 'data': qcode}]
        qmp.execute('send-key', {'keys': keys, 'hold-time': 35})
        time.sleep(0.055)
    qmp.execute('send-key', {'keys': [{'type': 'qcode', 'data': 'ret'}], 'hold-time': 40})
    time.sleep(0.8)


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: qemu_bash_smoke.py path/to/npkernel.iso')
    iso = pathlib.Path(sys.argv[1]).resolve()
    root = iso.parent
    with tempfile.TemporaryDirectory(prefix='npk-bash-qemu-') as temp:
        qmp_path = os.path.join(temp, 'qmp.sock')
        disk_path = os.path.join(temp, 'npk-disk.img')
        subprocess.run(['qemu-img', 'create', '-f', 'raw', disk_path, '32M'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        serial_path = os.path.join(root, 'qemu-bash-smoke.serial.log')
        screenshot_path = os.path.join(root, 'qemu-bash-smoke.ppm')
        symbol_path = os.path.join(root, 'qemu-bash-turkish-q-symbols.ppm')
        for path in (serial_path, screenshot_path, symbol_path):
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass
        command = [
            'qemu-system-x86_64', '-M', 'pc', '-m', '512M', '-smp', '2',
            '-cdrom', str(iso), '-drive', f'if=ide,format=raw,file={disk_path}',
            '-boot', 'd', '-display', 'none',
            '-serial', f'file:{serial_path}', '-qmp', f'unix:{qmp_path},server=on,wait=off',
            '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0',
            '-no-reboot', '-no-shutdown'
        ]
        process = subprocess.Popen(command, cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        qmp = None
        try:
            qmp = QMP(qmp_path)
            deadline = time.time() + 45
            while time.time() < deadline:
                serial = pathlib.Path(serial_path).read_text(errors='replace') if pathlib.Path(serial_path).exists() else ''
                if 'NPKernel initialization complete' in serial and 'launching ring3 pid' in serial:
                    break
                if process.poll() is not None:
                    fail('QEMU stopped before the shell launch log', serial_path, process)
                time.sleep(0.25)
            else:
                fail('timed out waiting for kernel and ring-3 shell launch', serial_path, process)
            time.sleep(1.0)

            # The prompt underscore must visibly alternate while stdin is idle.
            blink_paths = [os.path.join(temp, f'cursor-{index}.ppm') for index in range(9)]
            for index, path in enumerate(blink_paths):
                qmp.execute('human-monitor-command', {'command-line': f'screendump {path}'})
                if index + 1 < len(blink_paths): time.sleep(0.3)
            def ppm_pixels(path):
                data = pathlib.Path(path).read_bytes()
                header, separator, pixels = data.partition(b'\n255\n')
                if not separator or header.split()[:3] != [b'P6', b'1024', b'768']:
                    fail('cursor animation screenshot is not 1024x768 PPM', serial_path, process)
                return pixels
            first_frame = ppm_pixels(blink_paths[0])
            if not any(ppm_pixels(path) != first_frame for path in blink_paths[1:]):
                fail('prompt underscore did not blink between idle screenshots', serial_path, process)

            # Exercise stdin line discipline, stdout, cwd/chdir, getdents64,
            # uname, procfs-backed built-ins, and execve via the exec builtin.
            commands = (
                "echo *e ? ! ^ + % '&' / ( ) = _",
                'help',
                'echo npkshelltest',
                'pwd',
                'ls /bin',
                'cd /bin',
                'pwd',
                'ls',
                'uname -a',
                'ps',
                'free',
                "bash -c 'echo bash_c_mode_ok'",
                '/bin/bash.elf /bash-smoke.sh',
                'touch /persist/bash-empty.txt',
                'cp /bash-smoke.sh /persist/bash-copy.sh',
                'stat /persist/bash-copy.sh',
                'head -n 1 /persist/bash-copy.sh',
                'wc /persist/bash-copy.sh',
                'cat /persist/bash-copy.sh',
                "/bin/bash.elf -c 'touch /persist/bash-bg-ok' &",
                'jobs',
                'wait',
                'echo bash_smoke_done',
            )
            for index, line in enumerate(commands):
                send_command(qmp, line)
                if index == 0:
                    qmp.execute('human-monitor-command', {'command-line': f'screendump {symbol_path}'})
            time.sleep(2.0)
            qmp.execute('human-monitor-command', {'command-line': f'screendump {screenshot_path}'})
            time.sleep(0.5)
            serial = pathlib.Path(serial_path).read_text(errors='replace')
            if 'panic' in serial.lower() or 'exception' in serial.lower():
                fail('kernel reported a panic/exception during shell smoke', serial_path, process)
            if not pathlib.Path(screenshot_path).is_file() or pathlib.Path(screenshot_path).stat().st_size < 1024:
                fail('QEMU did not produce a non-empty shell framebuffer capture', serial_path, process)
            image_data = pathlib.Path(screenshot_path).read_bytes()
            header, separator, pixels = image_data.partition(b'\n255\n')
            if not separator or header.split()[:3] != [b'P6', b'1024', b'768'] or not any(pixels):
                fail('shell framebuffer is blank or not 1024x768 PPM', serial_path, process)
            if 'thread exit syscall completed; yielding to scheduler' not in serial:
                fail('exec builtin did not reach the ELF exit syscall', serial_path, process)
            if not pathlib.Path(symbol_path).is_file() or pathlib.Path(symbol_path).stat().st_size < 1024:
                fail('QEMU did not capture the Turkish-Q symbol echo', serial_path, process)
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()

        disk_image = pathlib.Path(disk_path).read_bytes()
        for marker in (b'bash-copy.sh', b'bash-bg-ok'):
            if marker not in disk_image:
                raise SystemExit(f'persistent filesystem did not retain expected marker {marker.decode()}')
        print(f'PASS: ring-3 shell booted; QMP USB keyboard injected {len(commands)} commands covering Turkish-Q symbols, foreground/background execution, Bash -c, script mode, and /persist file operations; framebuffer={screenshot_path}')
        print('Persistent disk evidence: copied script and background-created file names are present; no kernel panic/exception.')


if __name__ == '__main__':
    main()
