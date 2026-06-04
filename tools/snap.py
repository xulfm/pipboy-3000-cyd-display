#!/usr/bin/env python3
"""
snap.py — pull a pixel-perfect screenshot from the PIP-BOY (CYD) over USB.

Sends "SNAP" to trigger the device, which forces LVGL to redraw the whole
screen and streams each flushed strip back as base64 RGB565 rows tagged
with their position:  SCR-BEGIN W H 16 / SCRP x y w <b64> ... / SCR-END.
This script reassembles the strips into a PNG. Run while the device is
connected via USB and showing whatever you want to capture.

    python tools/snap.py                          # screenshot.png in cwd
    python tools/snap.py docs/home.png            # named output
    python tools/snap.py docs/home.png --port COM11
    python tools/snap.py x.png --swap             # flip byte order if colors look wrong

Requires: pyserial, Pillow.
    pip install pyserial pillow
"""
import argparse
import base64
import sys
import time
from pathlib import Path

import serial
from PIL import Image


def rgb565_to_rgb888(val: int):
    r = (val >> 11) & 0x1F
    g = (val >> 5) & 0x3F
    b = val & 0x1F
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


def grab(port: str, baud: int, out_path: Path, swap: bool, timeout: float = 25.0) -> None:
    s = serial.Serial(port, baud, timeout=2)
    try:
        s.reset_input_buffer()
        s.write(b"SNAP\n")
        s.flush()
        W = H = bpp = 0
        img = None
        px = None
        got_rows = 0
        deadline = time.time() + timeout
        last_send = time.time()
        started = False
        while time.time() < deadline:
            # Opening the CH340 port resets the board; if the first SNAP was
            # lost during boot, resend every 2s until the dump starts.
            if not started and time.time() - last_send > 2.0:
                s.write(b"SNAP\n")
                s.flush()
                last_send = time.time()
            raw = s.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip()
            if not line:
                continue
            if line.startswith("SCR-BEGIN"):
                parts = line.split()
                if len(parts) >= 4:
                    W, H, bpp = int(parts[1]), int(parts[2]), int(parts[3])
                if bpp != 16:
                    raise RuntimeError(f"unsupported color depth {bpp} (expected 16)")
                img = Image.new("RGB", (W, H), (0, 0, 0))
                px = img.load()
                got_rows = 0
                started = True
            elif line.startswith("SCRP ") and px is not None:
                # SCRP x y w <base64 row of w RGB565 pixels>
                try:
                    _, sx, sy, sw, b64 = line.split(" ", 4)
                    x0, y0, rw = int(sx), int(sy), int(sw)
                    row = base64.b64decode(b64)
                except Exception:
                    continue
                if y0 < 0 or y0 >= H:
                    continue
                for i in range(min(rw, (len(row) // 2))):
                    lo, hi = row[2 * i], row[2 * i + 1]
                    val = (hi << 8) | lo if not swap else (lo << 8) | hi
                    xx = x0 + i
                    if 0 <= xx < W:
                        px[xx, y0] = rgb565_to_rgb888(val)
                got_rows += 1
            elif line == "SCR-END":
                break
            elif line.startswith("SCR-ERR"):
                raise RuntimeError(line)
        else:
            raise TimeoutError("no SCR-END inside timeout window")

        if img is None or got_rows == 0:
            raise RuntimeError("no framebuffer data captured")
        img.save(out_path)
        print(f"saved {out_path} ({W}x{H}, {bpp} bpp, {got_rows} rows)")
    finally:
        s.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out", nargs="?", default="screenshot.png",
                    help="output PNG path (default: screenshot.png)")
    ap.add_argument("--port", default="COM11", help="serial port (default: COM11)")
    ap.add_argument("--baud", type=int, default=115200, help="baud rate")
    ap.add_argument("--swap", action="store_true",
                    help="swap RGB565 byte order (try if colors look wrong)")
    ap.add_argument("--timeout", type=float, default=25.0,
                    help="seconds to wait for SCR-END")
    args = ap.parse_args()
    try:
        grab(args.port, args.baud, Path(args.out), args.swap, args.timeout)
    except (RuntimeError, TimeoutError, serial.SerialException) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
