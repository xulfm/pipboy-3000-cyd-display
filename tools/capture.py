#!/usr/bin/env python3
"""
capture.py — interactive screenshot walkthrough for the PIP-BOY (CYD).

Holds ONE serial connection open for the whole session so the device
isn't reset between shots (opening the CH340 port resets the board, so
re-connecting per shot would bounce you back to the main menu). For each
screen it prints what to navigate to, waits for you to press Enter, then
triggers a capture and saves docs/<name>.png.

    python tools/capture.py --port COM11
    python tools/capture.py --port COM11 --only stat map radar
    python tools/capture.py --port COM11 --list

Controls at each prompt:
    <Enter>  capture this screen
    s        skip this screen
    r        (after a capture) re-take it
    q        quit

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

# (filename, on-device instruction). Order follows a natural tour.
SCREENS = [
    ("mainmenu",         "the MAIN MENU (boot screen)"),
    ("login",            "LOGIN: tap [ LOGIN ] from the main menu"),
    ("name_entry",       "NAME ENTRY: pick a dweller, tap [ ENTER VAULT ] (keyboard appears)"),
    ("stat",             "STAT tab (your dweller stats / S.P.E.C.I.A.L.)"),
    ("inv",              "INV tab (inventory)"),
    ("data",             "DATA tab (quests)"),
    ("map",              "MAP tab (wasteland overworld)"),
    ("radar",            "RADAR: on MAP, tap [ RADAR ] (shows DEMO/DISTANCE/LOCATING)"),
    ("radio",            "RADIO tab (mesh peers + chat)"),
    ("apps",             "APPS hub (tile grid)"),
    ("blackjack_menu",   "BLACKJACK: APPS -> BLACKJACK (mode select)"),
    ("blackjack_1p",     "BLACKJACK 1-player table mid-hand"),
    ("blackjack_lobby",  "BLACKJACK mesh HOST lobby (MESH HOST)"),
    ("blackjack_mesh",   "BLACKJACK multiplayer table (needs 2 devices)"),
    ("settings",         "SETTINGS (main panel)"),
    ("settings_anchor",  "Settings -> [ ANCHOR / LOCATION ] panel"),
    ("clock",            "CLOCK app (Chronometer)"),
    ("soundboard",       "SOUNDBOARD app"),
    ("holotape",         "HOLOTAPE app"),
    ("pomodoro",         "POMODORO app"),
]


def read_capture(s: serial.Serial, out_path: Path, timeout: float = 45.0) -> bool:
    s.reset_input_buffer()
    s.write(b"SNAP\n")
    s.flush()
    W = H = 0
    img = None
    px = None
    rows = 0
    deadline = time.time() + timeout
    while time.time() < deadline:
        raw = s.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip()
        if line.startswith("SCR-BEGIN"):
            p = line.split()
            W, H = int(p[1]), int(p[2])
            img = Image.new("RGB", (W, H), (0, 0, 0))
            px = img.load()
            rows = 0
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
                    r = ((val >> 11) & 0x1F) * 255 // 31
                    g = ((val >> 5) & 0x3F) * 255 // 63
                    b = (val & 0x1F) * 255 // 31
                    xx = x0 + i
                    if 0 <= xx < W:
                        px[xx, y0] = (r, g, b)
                rows += 1
        elif line == "SCR-END":
            break
    if img is None or rows == 0:
        print("  ! no data captured (is the device showing a screen?)")
        return False
    out_path.parent.mkdir(exist_ok=True)
    img.save(out_path)
    print(f"  -> saved docs/{out_path.name} ({W}x{H}, {rows} rows)")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", default="COM11")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--only", nargs="+", help="only these screen names")
    ap.add_argument("--list", action="store_true", help="list screen names and exit")
    args = ap.parse_args()

    if args.list:
        for name, instr in SCREENS:
            print(f"  {name:18s} {instr}")
        return 0

    targets = [s for s in SCREENS if (not args.only or s[0] in args.only)]
    if not targets:
        print("no matching screens", file=sys.stderr)
        return 1

    print(f"Opening {args.port} (the board will reset once, then boot)...")
    s = serial.Serial(args.port, args.baud, timeout=2)
    try:
        time.sleep(4.0)          # let the reset-on-open boot settle
        s.reset_input_buffer()
        print("Ready. Navigate on the device, then press Enter to capture.\n")
        for name, instr in targets:
            while True:
                ans = input(f"[{name}] {instr}\n  Enter=capture  s=skip  q=quit > ").strip().lower()
                if ans == "q":
                    print("done.")
                    return 0
                if ans == "s":
                    print("  (skipped)")
                    break
                ok = read_capture(s, DOCS / f"{name}.png")
                if ok:
                    again = input("  r=retake, Enter=next > ").strip().lower()
                    if again == "r":
                        continue
                break
    finally:
        s.close()
    print("\nAll done. Screenshots are in docs/.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
