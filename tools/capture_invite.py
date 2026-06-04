#!/usr/bin/env python3
"""
capture_invite.py — capture the incoming game-invite modal on a SECOND
device while a FIRST device hosts a mesh blackjack game.

Opens both serial ports up front (each board resets once on open, then
boots to the main menu), tells the host to broadcast a blackjack invite,
waits for the mesh to deliver it, then snaps the peer — which is now
showing the "INCOMING INVITE" modal.

    python tools/capture_invite.py --host-port COM11 --peer-port COM12

Requires: pyserial, Pillow.
"""
import argparse
import base64
import sys
import time
from pathlib import Path

import serial
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"


def snap(s: serial.Serial, name: str, timeout: float = 45.0) -> bool:
    s.reset_input_buffer()
    s.write(b"SNAP\n"); s.flush()
    W = H = 0; img = None; px = None; rows = 0
    deadline = time.time() + timeout
    while time.time() < deadline:
        raw = s.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip()
        if line.startswith("SCR-BEGIN"):
            p = line.split(); W, H = int(p[1]), int(p[2])
            img = Image.new("RGB", (W, H), (0, 0, 0)); px = img.load(); rows = 0
        elif line.startswith("SCRP ") and px is not None:
            try:
                _, sx, sy, sw, b64 = line.split(" ", 4)
                x0, y0, rw = int(sx), int(sy), int(sw)
                row = base64.b64decode(b64)
            except Exception:
                continue
            if 0 <= y0 < H:
                for i in range(min(rw, len(row) // 2)):
                    v = (row[2 * i + 1] << 8) | row[2 * i]
                    px[x0 + i, y0] = (((v >> 11) & 0x1F) * 255 // 31,
                                      ((v >> 5) & 0x3F) * 255 // 63,
                                      (v & 0x1F) * 255 // 31)
                rows += 1
        elif line == "SCR-END":
            break
    if img and rows:
        DOCS.mkdir(exist_ok=True)
        img.save(DOCS / f"{name}.png")
        print(f"  -> docs/{name}.png ({rows} rows)")
        return True
    print(f"  ! {name}: no data")
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host-port", default="COM11")
    ap.add_argument("--peer-port", default="COM12")
    ap.add_argument("--name", default="invite")
    ap.add_argument("--wait", type=float, default=10.0,
                    help="seconds to let the invite reach the peer")
    args = ap.parse_args()

    print(f"host={args.host_port}  peer={args.peer_port}")
    host = serial.Serial(args.host_port, 115200, timeout=2)
    peer = serial.Serial(args.peer_port, 115200, timeout=2)
    try:
        time.sleep(5.0)                       # both boot from the open-reset
        print("[host] broadcasting blackjack invite (NAV blackjackhost)")
        host.reset_input_buffer()
        host.write(b"NAV blackjackhost\n"); host.flush()
        print(f"[wait] {args.wait}s for the mesh to deliver the invite ...")
        time.sleep(args.wait)
        snap(peer, args.name)
    finally:
        host.close(); peer.close()
    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
