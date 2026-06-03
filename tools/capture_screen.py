#!/usr/bin/env python3
"""Capture the device RGB565 framebuffer over the debug serial port."""

from __future__ import annotations

import argparse
import base64
import os
import re
import select
import subprocess
import sys
import termios
import time
import tty
from pathlib import Path


def _ensure_serial_import() -> None:
    candidates = []
    env = Path(sys.prefix)
    candidates.extend(Path("/Users/bytedance/.espressif/python_env").glob("idf*_env/lib/python*/site-packages"))
    candidates.extend(env.glob("lib/python*/site-packages"))
    for path in candidates:
        if path.exists():
            sys.path.append(str(path))


_ensure_serial_import()

import serial  # type: ignore  # noqa: E402
from PIL import Image  # noqa: E402


BEGIN_RE = re.compile(r"__CODEX_SCREENSHOT_BEGIN__ width=(\d+) height=(\d+) format=([a-z0-9_-]+)")
ROW_RE = re.compile(r"__CODEX_SCREENSHOT_ROW__ y=(\d+) data=([A-Za-z0-9+/=]+)")


def rgb565_to_png(rows: list[bytes], width: int, height: int, output: Path, swap_bytes: bool) -> None:
    image = Image.new("RGB", (width, height))
    pixels = image.load()
    for y, row in enumerate(rows):
        if len(row) != width * 2:
            raise RuntimeError(f"row {y} has {len(row)} bytes, expected {width * 2}")
        for x in range(width):
            value = row[x * 2] | (row[x * 2 + 1] << 8)
            if swap_bytes:
                value = ((value & 0xFF) << 8) | (value >> 8)
            r = ((value >> 11) & 0x1F) * 255 // 31
            g = ((value >> 5) & 0x3F) * 255 // 63
            b = (value & 0x1F) * 255 // 31
            pixels[x, y] = (r, g, b)
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output)


class RawSerial:
    """Small POSIX serial reader/writer that avoids DTR/RTS ioctls."""

    def __init__(self, port: str, baud: int):
        self.port = port
        self.baud = baud
        self.fd: int | None = None
        self._buf = bytearray()
        self._old_attrs: list | None = None

    def __enter__(self) -> "RawSerial":
        _disable_hangup_on_close(self.port)
        self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self._old_attrs = termios.tcgetattr(self.fd)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        speed = _termios_baud(self.baud)
        if speed is not None:
            attrs[4] = speed
            attrs[5] = speed
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        return self

    def __exit__(self, _exc_type, _exc, _tb) -> None:
        if self.fd is not None:
            if self._old_attrs is not None:
                termios.tcsetattr(self.fd, termios.TCSANOW, self._old_attrs)
            os.close(self.fd)
            self.fd = None

    def write(self, data: bytes) -> None:
        assert self.fd is not None
        os.write(self.fd, data)
        termios.tcdrain(self.fd)

    def reset_input_buffer(self) -> None:
        if self.fd is not None:
            termios.tcflush(self.fd, termios.TCIFLUSH)
        self._buf.clear()

    def readline(self, timeout: float = 0.2) -> bytes:
        assert self.fd is not None
        deadline = time.time() + timeout
        while time.time() < deadline:
            for sep in (b"\n", b"\r"):
                pos = self._buf.find(sep)
                if pos >= 0:
                    line = bytes(self._buf[:pos])
                    del self._buf[: pos + 1]
                    return line
            remaining = max(0.0, deadline - time.time())
            ready, _, _ = select.select([self.fd], [], [], remaining)
            if not ready:
                continue
            chunk = os.read(self.fd, 4096)
            if chunk:
                self._buf.extend(chunk)
        return b""


def _termios_baud(baud: int) -> int | None:
    return getattr(termios, f"B{baud}", None)


def _disable_hangup_on_close(port: str) -> None:
    for args in (["stty", "-f", port, "-hupcl", "clocal"], ["stty", "-F", port, "-hupcl", "clocal"]):
        try:
            subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            return
        except OSError:
            return


def capture(
    port: str,
    output: Path,
    baud: int,
    timeout: float,
    settle: float,
    swap_bytes: bool,
    reset_lines: bool,
) -> None:
    rows: list[bytes] = []
    width = height = 0
    begin_at = 0.0
    started = False

    if reset_lines:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = baud
        ser.timeout = 0.2
        ser.dtr = False
        ser.rts = False
        port_io = ser
        mode = "pyserial with DTR/RTS low"
    else:
        port_io = RawSerial(port, baud)
        mode = "raw POSIX serial, no DTR/RTS ioctl"

    with port_io as ser:
        if settle > 0:
            print(f"waiting {settle:.1f}s for device UI ({mode})")
            time.sleep(settle)
        ser.reset_input_buffer()
        ser.write(b"codex:screenshot\n")
        if hasattr(ser, "flush"):
            ser.flush()

        deadline = time.time() + timeout
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if not line:
                continue
            begin = BEGIN_RE.search(line)
            if begin:
                width = int(begin.group(1))
                height = int(begin.group(2))
                fmt = begin.group(3)
                rows = [b""] * height
                begin_at = time.time()
                started = True
                print(f"capture started: {width}x{height} {fmt}")
                continue
            if "__CODEX_SCREENSHOT_END__" in line and started:
                missing = [i for i, row in enumerate(rows) if not row]
                if missing:
                    raise RuntimeError(f"missing {len(missing)} rows, first missing row {missing[0]}")
                rgb565_to_png(rows, width, height, output, swap_bytes)
                elapsed = time.time() - begin_at
                print(f"saved {output} ({elapsed:.1f}s)")
                return
            row_match = ROW_RE.search(line)
            if row_match and started:
                y = int(row_match.group(1))
                if 0 <= y < height:
                    rows[y] = base64.b64decode(row_match.group(2))
                    if y % 40 == 0:
                        print(f"row {y}/{height}")

    raise TimeoutError("timed out waiting for screenshot data")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/cu.usbmodem101")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle", type=float, default=8.0, help="seconds to wait after opening the serial port")
    parser.add_argument("--no-swap", action="store_true", help="decode raw little-endian RGB565 without byte swapping")
    parser.add_argument(
        "--reset-lines",
        action="store_true",
        help="use the old pyserial mode that drives DTR/RTS low; the default avoids DTR/RTS changes",
    )
    parser.add_argument("--output", default="/tmp/codexbuddy-screen.png")
    args = parser.parse_args()
    capture(args.port, Path(args.output), args.baud, args.timeout, args.settle, not args.no_swap, args.reset_lines)


if __name__ == "__main__":
    main()
