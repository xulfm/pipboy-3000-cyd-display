#!/usr/bin/env python3
"""
build_release.py — build both display variants and produce single-file,
flash-at-0x0 merged binaries in dist/.

Outputs:
    dist/pipboy-3000-cyd-ili9341-<ver>.bin   (env: cyd       — ILI9341)
    dist/pipboy-3000-cyd-st7789-<ver>.bin    (env: cyd_st7789 — ST7789)

Each merged image contains the bootloader, partition table, boot_app0,
and the application — flash it whole to offset 0x0, nothing else needed.

    pip install esptool
    python tools/build_release.py            # default version v1.0.0
    python tools/build_release.py --version v1.1.0
    python tools/build_release.py --only cyd

Requires PlatformIO Core (`pio`) on PATH and esptool (`pip install esptool`).
"""
import argparse
import glob
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"

VARIANTS = [
    {"env": "cyd",        "name": "ili9341"},
    {"env": "cyd_st7789", "name": "st7789"},
]


def pio_core_dir() -> Path:
    return Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))


def find_boot_app0() -> str:
    pats = [
        pio_core_dir() / "packages" / "framework-arduinoespressif32"
        / "tools" / "partitions" / "boot_app0.bin",
    ]
    for p in pats:
        if p.exists():
            return str(p)
    hits = glob.glob(str(pio_core_dir() / "packages" / "**" / "boot_app0.bin"),
                     recursive=True)
    if hits:
        return hits[0]
    raise FileNotFoundError("boot_app0.bin not found under the PlatformIO packages dir")


def run(cmd: list[str]) -> None:
    print("  $", " ".join(cmd))
    subprocess.run(cmd, check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--version", default="v1.0.0", help="version tag for filenames")
    ap.add_argument("--only", nargs="+", choices=[v["env"] for v in VARIANTS],
                    help="only build these envs")
    args = ap.parse_args()

    DIST.mkdir(exist_ok=True)
    boot_app0 = find_boot_app0()
    variants = [v for v in VARIANTS if not args.only or v["env"] in args.only]

    for v in variants:
        env = v["env"]
        print(f"\n=== building {env} ===")
        run(["pio", "run", "-e", env])
        build = ROOT / ".pio" / "build" / env
        out = DIST / f"pipboy-3000-cyd-{v['name']}-{args.version}.bin"
        print(f"=== merging -> {out.name} ===")
        run([sys.executable, "-m", "esptool", "--chip", "esp32", "merge_bin",
             "-o", str(out), "--flash_size", "4MB",
             "0x1000",  str(build / "bootloader.bin"),
             "0x8000",  str(build / "partitions.bin"),
             "0xe000",  boot_app0,
             "0x10000", str(build / "firmware.bin")])

    print("\nDone. Artifacts in dist/:")
    for f in sorted(DIST.glob("*.bin")):
        print(f"  {f.name}  ({f.stat().st_size / 1024 / 1024:.2f} MB)")
    print("\nFlash with:  esptool.py --chip esp32 --port COMx write_flash 0x0 <file>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
