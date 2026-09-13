#!/usr/bin/env python3
"""
Decode M913 configuration packets out of a USB capture.

Runs anywhere Python 3 does, Windows included — it needs no mouse and no
libusb. The point is to turn a .pcapng from the vendor software into something
readable, and in particular to diff two captures, which is how an unknown
field gets isolated: capture the same action twice with one thing changed, and
the only bytes that move are the ones encoding it.

Usage
-----
  decode-capture.py CAPTURE...                 decode each capture
  decode-capture.py --memory CAPTURE           show the memory image it writes
  decode-capture.py --diff BEFORE AFTER        show where two captures differ

CAPTURE is either

  * a .pcapng file — tshark is invoked for you if it is on PATH (or at the
    usual "C:\\Program Files\\Wireshark\\tshark.exe"), or
  * a text file of hex lines, as produced by

      tshark -r f.pcapng -Y "usb.data_len==17" -T fields -e usb.capdata

    Separators do not matter: "08:07:00..." and "08 07 00..." both parse, a
    line prefix is ignored, and lines that are not 17 bytes are skipped. The
    prefix rule means `m913-ctl --get > dump.txt` can be fed in directly, so
    the same diff works on memory read off the mouse:

      m913-ctl --get > before.txt     # then program a macro on Windows
      m913-ctl --get > after.txt
      decode-capture.py --diff before.txt after.txt

Every configuration packet is 17 bytes:

  [0] 0x08 host→device (0x09 is a reply)   [3..4] address, big-endian
  [1] sub-command: 07 write, 08 read, 04 commit, 01/02/03 handshake
  [5] payload length                       [6..15] payload
  [16] checksum: (0x55 - sum[0..15]) & 0xFF outbound,
                 (0x4C - sum[1..15]) & 0xFF inbound
"""

import argparse
import os
import shutil
import subprocess
import sys

# What is known to live where, from the write templates this tool already
# implements (src/protocol.cpp) plus the read sweep in src/protocol.h.
# Anything outside these ranges is the interesting part of a new capture.
ADDRESS_MAP = [
    (0x0000, 0x0001, "polling rate"),
    (0x0002, 0x0003, "active DPI stage count"),
    (0x0004, 0x000B, "unknown (read as 2 bytes at 0x0004 by the vendor sweep)"),
    (0x000C, 0x001F, "DPI slots 1-5, 4 bytes each"),
    (0x0020, 0x002B, "unknown"),
    (0x002C, 0x003F, "Areson: 'unknown_2' block sent after DPI"
                     " / Compx: per-stage LED colours"),
    (0x0040, 0x0053, "unknown"),
    (0x0054, 0x005D, "LED: colour, mode, brightness, speed"),
    (0x005E, 0x005F, "unknown"),
    (0x0060, 0x009F, "button mapping, 16 buttons x 4 bytes"),
    (0x00A0, 0x00FF, "unknown"),
    (0x0100, 0x02EF, "keyboard/consumer event lists, 16 buttons x 0x20"),
    (0x02F0, 0x0300, "unknown"),
    # 16 x 0x180 starting at 0x0301, so the last one runs to 0x1B00.
    (0x0301, 0x1B00, "16 regions of 384 bytes, read as erased flash (0xFF)"
                     " — PURPOSE UNKNOWN, prime macro candidate"),
]

SUBCOMMANDS = {
    0x01: "handshake/challenge",
    0x02: "echo",
    0x03: "status",
    0x04: "commit",
    0x07: "WRITE",
    0x08: "read",
}

TSHARK_CANDIDATES = [
    "tshark",
    r"C:\Program Files\Wireshark\tshark.exe",
    r"C:\Program Files (x86)\Wireshark\tshark.exe",
]


def region_of(addr):
    for lo, hi, name in ADDRESS_MAP:
        if lo <= addr <= hi:
            note = name
            if name.startswith("button mapping"):
                note += f"  (button {(addr - 0x0060) // 4})"
            elif name.startswith("keyboard"):
                note += f"  (button {(addr - 0x0100) // 0x20})"
            elif name.startswith("16 regions"):
                note += f"  (region {(addr - 0x0301) // 0x180})"
            return note
    return "outside every known range"


def is_hex(tok):
    try:
        int(tok, 16)
        return True
    except ValueError:
        return False


def find_tshark():
    for c in TSHARK_CANDIDATES:
        if os.path.isfile(c):
            return c
        found = shutil.which(c)
        if found:
            return found
    return None


def load(path):
    """Return a list of 17-byte packets from a .pcapng or a hex text file."""
    if path.lower().endswith((".pcapng", ".pcap")):
        tshark = find_tshark()
        if not tshark:
            sys.exit(f"{path}: tshark not found — install Wireshark, or convert "
                     f"the capture to hex text first (see --help)")
        out = subprocess.run(
            [tshark, "-r", path, "-Y", "usb.data_len==17",
             "-T", "fields", "-e", "usb.capdata"],
            capture_output=True, text=True)
        if out.returncode != 0:
            sys.exit(f"{path}: tshark failed: {out.stderr.strip()}")
        text = out.stdout
        if not text.strip():
            # Older/newer builds put the bytes in a different field.
            out = subprocess.run(
                [tshark, "-r", path, "-Y", "usb.data_len==17",
                 "-T", "fields", "-e", "usb.data_fragment"],
                capture_output=True, text=True)
            text = out.stdout
    else:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()

    packets = []
    for line in text.splitlines():
        hexes = line.replace(":", " ").replace(",", " ").split()
        # Take the last 17 tokens, so lines that carry a prefix parse too —
        # which is what lets `m913-ctl --get > dump.txt` be fed straight in
        # ("  [ 4] --> 08 08 00 ...", "       <-- 09 08 00 ...").
        if len(hexes) > 17:
            hexes = hexes[-17:]
        if len(hexes) != 17 or not all(len(h) == 2 and is_hex(h) for h in hexes):
            continue
        packets.append([int(h, 16) for h in hexes])
    return packets


def checksum_ok(p):
    if p[0] == 0x09:                       # device → host
        return p[16] == (0x4C - sum(p[1:16])) & 0xFF
    return p[16] == (0x55 - sum(p[0:16])) & 0xFF


def describe(p):
    direction = "<--" if p[0] == 0x09 else "-->"
    sub = SUBCOMMANDS.get(p[1], f"0x{p[1]:02x} UNKNOWN SUB-COMMAND")
    addr = (p[3] << 8) | p[4]
    length = p[5]
    payload = " ".join(f"{b:02x}" for b in p[6:6 + min(length, 10)])
    flag = "" if checksum_ok(p) else "  [BAD CHECKSUM]"
    return (f"{direction} {sub:<22} addr=0x{addr:04x} len={length:<3} "
            f"{payload:<30}{flag}\n"
            f"                                    {region_of(addr)}")


def memory_image(packets):
    """Apply every write to a sparse address→byte map."""
    mem = {}
    for p in packets:
        if p[0] != 0x08 or p[1] != 0x07:
            continue
        addr, length = (p[3] << 8) | p[4], p[5]
        for i in range(min(length, 10)):
            mem[addr + i] = p[6 + i]
    return mem


def print_memory(mem, title):
    print(f"\n=== {title}: {len(mem)} bytes written ===")
    if not mem:
        return
    run_start = None
    prev = None
    for addr in sorted(mem) + [None]:
        if run_start is None:
            run_start, prev = addr, addr
            continue
        if addr is not None and addr == prev + 1:
            prev = addr
            continue
        data = " ".join(f"{mem[a]:02x}" for a in range(run_start, prev + 1))
        print(f"  0x{run_start:04x}-0x{prev:04x}  {data}")
        print(f"                {region_of(run_start)}")
        run_start, prev = addr, addr


def main():
    ap = argparse.ArgumentParser(
        description="Decode M913 config packets from a USB capture.",
        epilog="Capture the same action twice with one setting changed, then "
               "--diff the two: the only bytes that move are the ones encoding "
               "it.")
    ap.add_argument("captures", nargs="+", metavar="CAPTURE")
    ap.add_argument("--memory", action="store_true",
                    help="show the memory image each capture writes")
    ap.add_argument("--diff", action="store_true",
                    help="compare exactly two captures byte by byte")
    args = ap.parse_args()

    if args.diff:
        if len(args.captures) != 2:
            sys.exit("--diff takes exactly two captures")
        a, b = (memory_image(load(p)) for p in args.captures)
        print(f"=== {args.captures[0]}  vs  {args.captures[1]} ===")
        addrs = sorted(set(a) | set(b))
        differing = [x for x in addrs if a.get(x) != b.get(x)]
        if not differing:
            print("  identical — the two sessions wrote the same bytes.\n"
                  "  If you expected a difference, the software may not have"
                  " written anything (did you press Apply?).")
            return
        for x in differing:
            av = f"{a[x]:02x}" if x in a else "--"
            bv = f"{b[x]:02x}" if x in b else "--"
            print(f"  0x{x:04x}  {av} -> {bv}   {region_of(x)}")
        print(f"\n  {len(differing)} byte(s) differ, "
              f"{len(addrs) - len(differing)} identical.")
        return

    for path in args.captures:
        packets = load(path)
        print(f"\n=== {path}: {len(packets)} 17-byte packet(s) ===")
        if not packets:
            print("  Nothing here. The capture has no 17-byte transfers in it:"
                  " wrong USBPcap interface, or Apply was never pressed.")
            continue
        for p in packets:
            print(describe(p))
        bad = sum(1 for p in packets if not checksum_ok(p))
        if bad:
            print(f"\n  {bad} packet(s) failed their checksum — suspect the"
                  " capture, not the mouse.")
        if args.memory:
            print_memory(memory_image(packets), path)


if __name__ == "__main__":
    main()
