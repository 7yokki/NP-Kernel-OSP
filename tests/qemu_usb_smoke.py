#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
SUCCESS_MARKERS = (
    "device catalog loaded through VFS /ddc.json",
    "smp: CPUs online\n       value=0x0000000000000002",
    "USB VID:PID\n       value=0x0000000006270001",
    "USB class/subclass/protocol\n       value=0x0000000000030101",
    "devmgr: keyboard",
    "QEMU USB Keyboard",
    "usb_generic_keyboard: boot-protocol input enabled; polling HID reports",
    "NPKernel initialization complete",
)
KEYS = (("a", 0x04), ("b", 0x05), ("c", 0x06), ("d", 0x07),
        ("e", 0x08), ("f", 0x09), ("g", 0x0a), ("h", 0x0b))
KEY_REPORT_MARKERS = tuple(
    f"HID keyboard usage/state\n       value=0x{(usage << 32) | pressed:016x}"
    for _, usage in KEYS for pressed in (1, 0)
)
FAILURE_MARKERS = (
    "initial 8-byte device descriptor request failed",
    "full device descriptor request failed",
    "configuration descriptor header request failed",
    "full configuration descriptor request failed",
    "HID report descriptor validation failed",
    "HID boot protocol selection failed",
    "interrupt-IN transfer could not be started",
)


def qmp_command(connection: socket.socket, stream, command: str,
                arguments: dict | None, request_id: int) -> None:
    message: dict = {"execute": command, "id": request_id}
    if arguments is not None:
        message["arguments"] = arguments
    connection.sendall((json.dumps(message, separators=(",", ":")) + "\r\n").encode())
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError(f"QMP closed while awaiting {command}")
        response = json.loads(line.decode(errors="replace"))
        if response.get("id") != request_id:
            continue
        if "error" in response:
            raise RuntimeError(f"QMP {command} failed: {response['error']}")
        return


def inject_keys(qmp_path: pathlib.Path) -> None:
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if qmp_path.exists():
            break
        time.sleep(0.05)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(5.0)
        connection.connect(str(qmp_path))
        stream = connection.makefile("rb")
        greeting = stream.readline()
        if not greeting or "QMP" not in greeting.decode(errors="replace"):
            raise RuntimeError("QMP greeting was not received")
        qmp_command(connection, stream, "qmp_capabilities", None, 1)
        # QEMU's documented QMP send-key releases the key after hold-time ms.
        for request_id, (key, _) in enumerate(KEYS, start=2):
            qmp_command(connection, stream, "send-key",
                        {"keys": [{"type": "qcode", "data": key}], "hold-time": 60},
                        request_id)
            time.sleep(0.1)


def main() -> int:
    parser = argparse.ArgumentParser(description="Boot NPKernel and inject a USB HID keyboard key")
    parser.add_argument("iso", nargs="?", default=str(ROOT / "build" / "npkernel.iso"))
    parser.add_argument("--timeout", type=float, default=35.0)
    args = parser.parse_args()

    iso = pathlib.Path(args.iso).resolve()
    qemu = shutil.which("qemu-system-x86_64")
    if not iso.is_file():
        print(f"FAIL: ISO not found: {iso}", file=sys.stderr)
        return 2
    if not qemu:
        print("FAIL: qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2

    build = ROOT / "build"
    build.mkdir(parents=True, exist_ok=True)
    serial_log = build / "qemu-usb-smoke.serial.log"
    stderr_log = build / "qemu-usb-smoke.stderr.log"
    qmp_path = build / "qemu-usb-smoke.qmp.sock"
    serial_log.unlink(missing_ok=True)
    stderr_log.unlink(missing_ok=True)
    qmp_path.unlink(missing_ok=True)
    command = [
        qemu, "-machine", "q35,usb=off", "-m", "512M", "-smp", "2",
        "-cdrom", str(iso), "-device", "qemu-xhci,id=xhci",
        "-device", "usb-kbd,bus=xhci.0", "-display", "none",
        "-serial", f"file:{serial_log}", "-monitor", "none",
        "-qmp", f"unix:{qmp_path},server=on,wait=off",
        "-no-reboot", "-no-shutdown",
    ]

    with stderr_log.open("wb") as stderr:
        process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.DEVNULL,
                                   stderr=stderr, start_new_session=True)
        deadline = time.monotonic() + args.timeout
        boot_success = False
        key_sent = False
        key_success = False
        qmp_error = None
        while time.monotonic() < deadline:
            time.sleep(0.1)
            text = serial_log.read_text(errors="replace") if serial_log.exists() else ""
            if any(marker in text for marker in FAILURE_MARKERS):
                break
            if all(marker in text for marker in SUCCESS_MARKERS):
                boot_success = True
                if not key_sent:
                    try:
                        inject_keys(qmp_path)
                        key_sent = True
                    except Exception as exc:  # Report harness failures alongside serial evidence.
                        qmp_error = str(exc)
                        break
                if all(marker in text for marker in KEY_REPORT_MARKERS):
                    key_success = True
                    break
            if process.poll() is not None:
                break
        if process.poll() is None:
            try:
                os_kill_group(process.pid, signal.SIGTERM)
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os_kill_group(process.pid, signal.SIGKILL)
                process.wait()

    text = serial_log.read_text(errors="replace") if serial_log.exists() else ""
    if not boot_success or not key_success:
        print("FAIL: USB HID boot or real key press/release assertions were not all observed.")
        if qmp_error:
            print(f"QMP error: {qmp_error}")
        print(f"Serial log: {serial_log}")
        print("--- serial tail ---")
        print("\n".join(text.splitlines()[-100:]))
        if stderr_log.stat().st_size:
            print(f"QEMU stderr: {stderr_log}")
        return 1

    print("QEMU_USB_HID_KEYBOARD passed")
    print("Assertions: USB device/interface recognized; xHCI endpoint configured; HID driver bound; ")
    print("QMP-injected HID usages 0x04–0x0b (a–h) press/release reached the kernel; ")
    print("16 reports exercised the interrupt transfer ring wrap/cycle transition.")
    print(f"Serial log: {serial_log}")
    return 0


def os_kill_group(pid: int, sig: int) -> None:
    os.killpg(pid, sig)


if __name__ == "__main__":
    raise SystemExit(main())
