#!/usr/bin/env python3
"""
auto_capture.py — drive the PIP-BOY through every screen over USB serial
and save a PNG for each into docs/. No touchscreen interaction needed:
the firmware exposes "NAV <screen>" commands (and "SNAP") on serial.

    python tools/auto_capture.py --port COM11

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

# Tour: (kind, arg). kind = nav | snap | reset | wait
TOUR = [
    ("wait", 5.0),                 # let the open-reset boot settle
    ("snap", "mainmenu"),
    ("nav", "apps"),          ("snap", "apps"),
    ("nav", "blackjackmenu"), ("snap", "blackjack_menu"),
    ("nav", "blackjack1p"),   ("snap", "blackjack_1p"),
    ("nav", "pomodoro"),      ("snap", "pomodoro"),
    ("nav", "soundboard"),    ("snap", "soundboard"),
    ("nav", "holotape"),      ("snap", "holotape"),
    ("nav", "dice"),          ("snap", "dice"),
    ("nav", "quests"),        ("snap", "quests"),
    ("nav", "status"),        ("snap", "status"),
    ("nav", "settings"),      ("snap", "settings"),
    ("nav", "anchor"),        ("snap", "settings_anchor"),
    ("nav", "clock"),         ("snap", "clock"),
    # reboot to a clean main menu, then do the login + tab tour
    ("reset", None),          ("wait", 6.0),
    ("nav", "login"),         ("snap", "login"),
    ("nav", "autologin"),     ("snap", "stat"),
    ("nav", "inv"),           ("snap", "inv"),
    ("nav", "data"),          ("snap", "data"),
    ("nav", "map"),           ("snap", "map"),
    ("nav", "radar"),         ("snap", "radar"),
    ("nav", "radio"),         ("snap", "radio"),
]


def do_snap(s: serial.Serial, name: str, timeout: float = 45.0) -> bool:
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
                    val = (row[2 * i + 1] << 8) | row[2 * i]
                    px[x0 + i, y0] = (((val >> 11) & 0x1F) * 255 // 31,
                                      ((val >> 5) & 0x3F) * 255 // 63,
                                      (val & 0x1F) * 255 // 31)
                rows += 1
        elif line == "SCR-END":
            break
    if img is None or rows == 0:
        print(f"  ! {name}: no data"); return False
    DOCS.mkdir(exist_ok=True)
    img.save(DOCS / f"{name}.png")
    print(f"  -> docs/{name}.png ({rows} rows)")
    return True


# Per-screen nav recipe from a fresh boot (for --only re-captures). Each
# screen is reached independently after a reset, so re-snapping one screen
# never disturbs the others (e.g. preserving a radio shot with a live peer).
RECIPES = {
    "mainmenu": [],
    "apps": ["apps"], "blackjack_menu": ["apps", "blackjackmenu"],
    "blackjack_1p": ["apps", "blackjack1p"], "pomodoro": ["apps", "pomodoro"],
    "soundboard": ["apps", "soundboard"], "holotape": ["apps", "holotape"],
    "dice": ["apps", "dice"], "quests": ["apps", "quests"],
    "status": ["apps", "status"], "settings": ["apps", "settings"],
    "settings_anchor": ["apps", "anchor"], "clock": ["apps", "clock"],
    "login": ["login"], "name_entry": ["login", "nameentry"],
    "blackjack_lobby": ["apps", "blackjackhost"],
    "stat": ["login", "autologin"],
    "inv": ["login", "autologin", "inv"], "data": ["login", "autologin", "data"],
    "map": ["login", "autologin", "map"], "radar": ["login", "autologin", "radar"],
    "radio": ["login", "autologin", "radio"],
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM11")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--only", nargs="+", help="re-capture only these screens "
                    "(each reached independently from a reset)")
    args = ap.parse_args()

    print(f"Opening {args.port} ...")
    s = serial.Serial(args.port, args.baud, timeout=2)

    if args.only:
        try:
            time.sleep(4.0)
            for name in args.only:
                if name not in RECIPES:
                    print(f"  ! unknown screen: {name}"); continue
                print(f"[{name}] reset + {RECIPES[name]}")
                s.write(b"NAV reset\n"); s.flush(); time.sleep(6.0)
                for cmd in RECIPES[name]:
                    s.reset_input_buffer()
                    s.write(f"NAV {cmd}\n".encode()); s.flush(); time.sleep(0.9)
                do_snap(s, name)
        finally:
            s.close()
        print("\nDone.")
        return 0

    try:
        for kind, arg in TOUR:
            if kind == "wait":
                time.sleep(arg)
            elif kind == "nav":
                print(f"[nav {arg}]")
                s.reset_input_buffer()
                s.write(f"NAV {arg}\n".encode()); s.flush()
                time.sleep(0.9)          # let the screen build + render
            elif kind == "reset":
                print("[reset]")
                s.write(b"NAV reset\n"); s.flush()
                time.sleep(0.5)
            elif kind == "snap":
                do_snap(s, arg)
    finally:
        s.close()
    print("\nDone. Screenshots in docs/.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
