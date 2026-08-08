#!/usr/bin/env python3
"""
Interactive hardware verification for m913-ctl.

Decoder is corrected from the first attempt. The report layout was confirmed
empirically on 25a7:fa07:

    keyboard  EP 0x82:  [0]=0x01 report ID, [1]=modifiers, [2..7]=keycodes
    consumer  EP 0x82:  [0]=0x05 report ID, [1]=usage
    mouse     EP 0x81:  [0]=buttons bitmask, [1..]=movement

The earlier version folded byte[0] into the modifier mask (so every button
reported a phantom ctrl) and scanned every byte for keycodes (so the super
modifier, 0x08, was misread as the keycode for 'e'). Both are fixed by
decoding at fixed offsets keyed off the report ID.

Media keys are checked twice: by decoding the consumer report while the tool
holds the device, and again after it lets go -- they physically cannot reach
the OS while m913-ctl has the interface claimed, which is what made the first
run report a false failure.

Usage:  python3 tests/verify-hardware.py [path-to-m913-ctl]
"""

import os
import re
import subprocess
import sys
import threading
import time
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CTL = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                      else os.environ.get("CTL", os.path.join(REPO, "build", "m913-ctl")))
TEST_INI = os.path.join(HERE, "verify-hardware.ini")
# Re-applied at the end so the mouse is left as it was found. Override with
# RESTORE_INI=~/my.ini if you keep your real config elsewhere.
RESTORE_INI = os.environ.get("RESTORE_INI", os.path.join(REPO, "examples", "example.ini"))

B, D, R, G, Y, C, X = ("\033[1m", "\033[2m", "\033[31m", "\033[32m",
                       "\033[33m", "\033[36m", "\033[0m")
results = []


def record(phase, name, ok, detail=""):
    results.append((phase, name, ok, detail))
    print(f"    [{G}PASS{X}]" if ok else f"    [{R}FAIL{X}]",
          name, f"{D}{detail}{X}" if detail else "")


def header(t):
    print(f"\n{B}{C}{'=' * 70}{X}\n{B}{C}  {t}{X}\n{B}{C}{'=' * 70}{X}")


def ask(q):
    while True:
        a = input(f"    {Y}?{X} {q} [y/n/s=skip] ").strip().lower()
        if a.startswith("y"): return True
        if a.startswith("n"): return False
        if a.startswith("s"): return None


def run(args):
    p = subprocess.run([CTL] + args, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


KEYS = {0x04:"a",0x05:"b",0x06:"c",0x07:"d",0x08:"e",0x09:"f",0x0a:"g",0x0b:"h",
        0x0c:"i",0x0d:"j",0x0e:"k",0x0f:"l",0x10:"m",0x11:"n",0x12:"o",0x13:"p",
        0x14:"q",0x15:"r",0x16:"s",0x17:"t",0x18:"u",0x19:"v",0x1a:"w",0x1b:"x",
        0x1c:"y",0x1d:"z",0x1e:"1",0x1f:"2",0x20:"3",0x21:"4",0x22:"5",0x23:"6",
        0x24:"7",0x25:"8",0x26:"9",0x27:"0",0x28:"enter",0x29:"esc",
        0x2a:"backspace",0x2b:"tab",0x2c:"space",0x3e:"f5",0x57:"numplus",
        0x4f:"arrow_right",0x50:"arrow_left",0x51:"arrow_down",0x52:"arrow_up"}
MODS = [(0x01,"ctrl"),(0x02,"shift"),(0x04,"alt"),(0x08,"super"),
        (0x10,"ctrl_r"),(0x20,"shift_r"),(0x40,"alt_r"),(0x80,"super_r")]
CONSUMER = {0xcd:"media_play",0xe9:"media_vol_up",0xea:"media_vol_down",
            0xb5:"media_next",0xb6:"media_prev",0xe2:"media_mute"}

RPT_KEYBOARD, RPT_CONSUMER = 0x01, 0x05
LINE = re.compile(r"\[pkt \d+ \| EP 0x(\w+) \| (\d+)B\]\s+([0-9a-f ]+)")


def mods_str(m):
    return "+".join(n for b, n in MODS if m & b) or "none"


class Listener:
    def __init__(self):
        self.proc, self.lock, self.reports = None, threading.Lock(), []

    def start(self):
        self.proc = subprocess.Popen([CTL, "--listen"], stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.proc.stdout:
            m = LINE.search(line)
            if m:
                with self.lock:
                    self.reports.append((int(m.group(1), 16),
                                         [int(x, 16) for x in m.group(3).split()]))

    def drain(self):
        with self.lock:
            r = list(self.reports); self.reports.clear()
        return r

    def stop(self):
        if self.proc:
            # SIGTERM. Before the signal fix this killed the process outright,
            # leaving the kernel driver detached and the mouse dead until
            # replug -- so reaching the end of this script with a working
            # mouse is itself the test.
            self.proc.terminate()
            try: self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired: self.proc.kill()


def collect(lis, button, expect):
    print(f"\n  {B}{button}{X} -> expecting {B}{expect}{X}")
    input(f"    Press and release {B}{button}{X} 2-3 times, then Enter... ")
    keys, mods, usages, raws, clicks = set(), 0, set(), [], 0
    for ep, d in lis.drain():
        if ep == 0x81:
            if d and d[0] & 0x01: clicks += 1
            continue
        raws.append(" ".join(f"{b:02x}" for b in d))
        if not d: continue
        if d[0] == RPT_KEYBOARD and len(d) >= 3:
            mods |= d[1]
            keys |= {b for b in d[2:] if b}
        elif d[0] == RPT_CONSUMER and len(d) >= 2:
            if d[1]: usages.add(d[1])
    return keys, mods, usages, raws, clicks


# button, label, expected keycodes, expected modifier mask, note
BUTTONS = [
    ("side1",  "ctrl+shift+z", {0x1d},             0x03, "3 tokens - PR #5 boundary"),
    ("side2",  "ctrl+a+b",     {0x04, 0x05},       0x01, "3 tokens - PR #5 boundary"),
    ("side3",  "a+b+c",        {0x04, 0x05, 0x06}, 0x00, "3 tokens - PR #5 boundary"),
    ("side4",  "super",        set(),              0x08, "modifier-only (was misread as 'e')"),
    ("side5",  "ctrl+c",       {0x06},             0x01, "2 tokens"),
    ("side6",  "f5",           {0x3e},             0x00, "plain key"),
    ("side8",  "shift+tab",    {0x2b},             0x02, "2 tokens"),
    ("side9",  "arrow_left",   {0x50},             0x00, "NEW alias - not the mouse button"),
    ("side10", "numplus",      {0x57},             0x00, "plain key"),
]


def main():
    if not os.path.exists(CTL):
        print(f"{R}m913-ctl not found at {CTL}{X}"); sys.exit(1)
    print(f"{B}m913-ctl interactive verification{X}\n  binary: {CTL}\n  config: {TEST_INI}")
    print(f"\n{Y}Note:{X} the cursor freezes while the mouse is grabbed. Keyboard only.")
    input("\nPress Enter to begin...")

    header("PHASE 1  Apply config")
    rc, out = run(["--config", TEST_INI])
    if rc != 0:
        record("apply", "config applied", False, out.strip().splitlines()[-1])
        print(f"\n{R}Stopping.{X}"); sys.exit(1)
    npkt = len(re.findall(r"^\s+--> ", out, re.M))
    noack = out.count("no ACK")
    record("apply", "config applied", True, f"{npkt} packets")
    record("apply", "every packet ACKed", noack == 0,
           "all ACKed" if noack == 0 else f"{noack} without ACK (wireless latency)")

    header("PHASE 2  LED (visual)")
    print("  Config set LED = steady red. The colour line in the .ini has an")
    print("  inline ';' comment -- if that were not stripped, this would fail.\n")
    for mode, desc in [("steady", "solid RED"), ("respiration", "breathing/pulsing"),
                       ("rainbow", "cycling colours"), ("off", "completely OFF")]:
        if mode != "steady": run(["--led", mode])
        r = ask(f"Is the LED {B}{desc}{X}?")
        if r is not None: record("led", f"--led {mode}", r, desc)
    run(["--config", TEST_INI])

    header("PHASE 3  DPI stages (visual)")
    print("  side12 = dpi-cycle. Slots are 400/1600/3200/6400/12000.\n")
    print(f"  {Y}Press side12 a few times, moving the mouse between presses.{X}")
    r = ask("Do you get 5 distinct cursor speeds?")
    if r is not None: record("dpi", "5 DPI stages cycle", r)

    header("PHASE 4  Buttons (decoded from the mouse's own reports)")
    lis = Listener(); lis.start(); time.sleep(1.5); lis.drain()
    for btn, combo, ekeys, emods, note in BUTTONS:
        keys, mods, _, raws, _ = collect(lis, btn, f"{combo}   ({note})")
        if not raws:
            record("buttons", f"{btn} = {combo}", False, "no reports captured"); continue
        ok = ekeys.issubset(keys) and (mods & emods) == emods and \
             (not ekeys or True) and (len(keys) == len(ekeys))
        detail = f"keys={sorted(KEYS.get(k, hex(k)) for k in keys) or '[]'} mods={mods_str(mods)}"
        record("buttons", f"{btn} = {combo}", ok, detail)
        if not ok:
            for r_ in raws[:4]: print(f"        {D}raw: {r_}{X}")

    header("PHASE 5  Multimedia")
    for btn, label, usage in [("side7", "media_vol_up", 0xe9), ("side11", "media_play", 0xcd)]:
        _, _, usages, raws, _ = collect(lis, btn, f"{label} (consumer usage 0x{usage:02x})")
        record("media", f"{btn} transmits {label}", usage in usages,
               f"usages seen: {[CONSUMER.get(u, hex(u)) for u in usages] or '[]'}")

    header("PHASE 6  Fire button")
    _, _, _, _, clicks = collect(lis, "fire", "fire:58:3 - three rapid left clicks")
    record("buttons", "fire = fire:58:3", clicks >= 2, f"{clicks} button-down edges on EP 0x81")

    lis.stop(); time.sleep(1.0)

    header("PHASE 7  Media out-of-band + SIGTERM survival")
    print("  The listener was stopped with SIGTERM. Before the signal fix that")
    print("  left the kernel driver detached and the mouse dead. If the mouse")
    print("  still works below, the fix holds.\n")
    input(f"    Press {B}side7{X} (volume up) a few times, then Enter... ")
    r = ask("Did the system volume actually change?")
    if r is not None:
        record("media", "side7 reaches the OS after SIGTERM", r,
               "also proves the driver was reattached")
    r = ask("Does the mouse cursor move normally now?")
    if r is not None:
        record("signals", "mouse alive after SIGTERM", r)

    header("SUMMARY")
    byp = defaultdict(list)
    for p, n, ok, d in results: byp[p].append((n, ok, d))
    for p in ("apply", "led", "dpi", "buttons", "media", "signals"):
        if p not in byp: continue
        print(f"\n  {B}{p}{X}")
        for n, ok, d in byp[p]:
            print(f"    {G}ok  {X}" if ok else f"    {R}FAIL{X}", n,
                  f"{D}{d}{X}" if d else "")
    total = len(results)
    good = sum(1 for r in results if r[2])
    print(f"\n  {B}{good}/{total} checks passed{X}")
    crit = [n for p, n, ok, _ in results
            if not ok and any(n.startswith(b) for b in ("side1 ", "side2 ", "side3 "))]
    if crit:
        print(f"  {R}Boundary cases FAILED: {', '.join(crit)}{X}")
    elif good == total:
        print(f"  {G}Everything passed.{X}")

    header("RESTORE")
    if ask(f"Re-apply {RESTORE_INI}?"):
        rc, _ = run(["--config", RESTORE_INI])
        print("    restored" if rc == 0 else f"    {R}RESTORE FAILED{X}")
    else:
        print(f"    Restore later with:\n      {CTL} --config {RESTORE_INI}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n{Y}Interrupted.{X} The mouse may still hold the test config.")
