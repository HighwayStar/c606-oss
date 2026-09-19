#!/usr/bin/env python3
"""Flash the PoC app into the vendor's ota_0 slot without touching the
bootloader, partition table or NVS.

  1. reads the partition table from the device (0x8000, or scans for it)
  2. (unless --no-backup) dumps the regions it is about to modify (ota_0 and
     otadata) to backup-<timestamp>-<name>.bin; --full-backup dumps all flash
  3. writes the image (build/c606_oss.bin, or build-c706/c706_oss.bin with
     --board c706 / when only that one exists) at the ota_0 offset
  4. erases otadata so the bootloader falls back to ota_0

The partition table is read from the device, so the same helper works for
the C606 (16 MB flash, ota_0 @ 0x20000) and the C706 (32 MB, ota_0 @ 0x20000).

Restore the vendor firmware later with:
  tools/flash_poc.py -p PORT --restore <vendor_ota_0.bin>

Requires esptool (comes with ESP-IDF: run inside `. export.sh`).
"""
import argparse, os, struct, subprocess, sys, tempfile, time

BOARDS = ("c606", "c606pro", "c706")

def default_app(board=None):
    """<board>_oss.bin next to this script (release zip) or in the source tree's
    build dir (build/ for the C606, build-<board>/ otherwise)."""
    here = os.path.dirname(os.path.abspath(__file__))
    for b in ([board] if board else BOARDS):
        bdir = "build" if b == "c606" else f"build-{b}"
        for c in (os.path.join(here, f"{b}_oss.bin"), os.path.join(here, "..", bdir, f"{b}_oss.bin"), f"{bdir}/{b}_oss.bin"):
            if os.path.exists(c):
                return c
    b = board or "c606"
    return f"{'build' if b == 'c606' else 'build-' + b}/{b}_oss.bin"

def esptool(port, *args, after="no_reset"):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port, "--after", after] + list(args)
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)

def read_flash(port, off, size, out):
    esptool(port, "read_flash", hex(off), hex(size), out)

def parse_parttable(data):
    ents = {}
    for base in [0x8000, 0x7000, 0x9000] + list(range(0, 0x10000, 0x1000)):
        if data[base:base + 2] != b"\xaa\x50":
            continue
        p = base
        while data[p:p + 2] == b"\xaa\x50":
            typ, sub, off, size = struct.unpack("<BBII", data[p + 2:p + 12])
            name = data[p + 12:p + 28].split(b"\0")[0].decode(errors="replace")
            ents[name] = dict(type=typ, sub=sub, off=off, size=size)
            p += 32
        return base, ents
    return None, ents

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", required=True)
    ap.add_argument("--board", choices=BOARDS, help="which image to look for (default: the first one found)")
    ap.add_argument("--app", help="firmware image (default: <board>_oss.bin next to this script or in the build dir)")
    ap.add_argument("--no-backup", action="store_true")
    ap.add_argument("--full-backup", action="store_true", help="dump the whole flash instead of just ota_0/otadata")
    ap.add_argument("--flash-size", default="ALL", help="for --full-backup (esptool detects it; C606 16MB, C706 32MB)")
    ap.add_argument("--restore", metavar="VENDOR_OTA0_BIN", help="write this image to ota_0 instead of the PoC")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    img = a.restore or a.app or default_app(a.board)
    if not os.path.exists(img):
        sys.exit(f"{img} not found (build first, or keep <board>_oss.bin next to this script)")

    with tempfile.TemporaryDirectory() as td:
        pt = os.path.join(td, "pt.bin")
        read_flash(a.port, 0, 0x10000, pt)
        base, ents = parse_parttable(open(pt, "rb").read())
    if base is None:
        sys.exit("no partition table found in the first 64 KB - refusing to guess")
    print(f"partition table @ {base:#x}:")
    for n, e in ents.items():
        print(f"  {n:16s} type={e['type']} sub={e['sub']:#04x} off={e['off']:#09x} size={e['size']:#09x}")

    app_slot = ents.get("ota_0") or ents.get("factory") or ents.get("app0")
    if not app_slot or app_slot["type"] != 0:
        sys.exit("no ota_0/factory app partition found")
    otadata = next((e for e in ents.values() if e["type"] == 1 and e["sub"] == 0), None)

    sz = os.path.getsize(img)
    if sz > app_slot["size"]:
        sys.exit(f"{img} ({sz} bytes) does not fit into the app slot ({app_slot['size']} bytes)")
    print(f"\nwill write {img} ({sz} bytes) to {app_slot['off']:#x}"
          + (f" and erase otadata @ {otadata['off']:#x}" if otadata else " (no otadata partition)"))
    if a.dry_run:
        return

    if not a.no_backup:
        stamp = time.strftime("backup-%Y%m%d-%H%M%S")
        if a.full_backup:
            print(f"\nfull flash backup -> {stamp}.bin (slow)")
            esptool(a.port, "read_flash", "0", a.flash_size, f"{stamp}.bin")
        else:
            regions = [("ota_0", app_slot)] + ([("otadata", otadata)] if otadata else [])
            for name, e in regions:
                out = f"{stamp}-{name}.bin"
                print(f"\nbackup {name} -> {out}")
                esptool(a.port, "read_flash", hex(e["off"]), hex(e["size"]), out)

    esptool(a.port, "write_flash", hex(app_slot["off"]), img)
    if otadata:
        esptool(a.port, "erase_region", hex(otadata["off"]), hex(otadata["size"]))
    esptool(a.port, "chip_id", after="hard_reset")
    print("\ndone - device reset")

if __name__ == "__main__":
    main()
