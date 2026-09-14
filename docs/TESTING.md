# Testing and development

This project talks to hardware over raw USB, so most of what can go wrong shows
up only on a real mouse — and often silently, since a malformed packet is
usually accepted and simply does the wrong thing. The two scripts in
[`tests/`](../tests) exist to make those failures visible.

| | What it covers | Needs a mouse? |
|---|---|---|
| [`tests/regress.sh`](../tests/regress.sh) | Packet building, read-back decoding, validation, config parsing, CLI surface, signal handling | Partly — device sections skip themselves |
| [`tests/verify-hardware.py`](../tests/verify-hardware.py) | What the mouse actually transmits after being configured | Yes, and a human to press buttons |
| [`tools/decode-capture.py`](../tools/decode-capture.py) | Not a test — decodes a USB capture or a `--get` dump, and diffs two of them | No |

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The build must stay clean under `-Wall -Wextra`; `regress.sh` fails if it isn't.

## Automated regression

```bash
./tests/regress.sh                  # everything available
OFFLINE_ONLY=1 ./tests/regress.sh   # never open the device
RESTORE_INI=~/my.ini ./tests/regress.sh
```

The offline half needs no hardware and is safe to run anywhere. It compiles
small harnesses against the real sources and checks behaviour directly:

- **Key-combo capacity**, built with ASan and `_GLIBCXX_ASSERTIONS`. A binding
  is stored as a list of HID events split across two 17-byte sub-packets, which
  fit 18 event bytes — three modifiers-and-keys total. Going over used to write
  past the end of a `std::array` and corrupt the stack, so the suite asserts
  that 3-token combos still build and 4-token ones are refused.
- **Compx DPI stage count**, checked against the hardcoded cascade it was
  refactored from, across all 32 `enabled[]` patterns. Any difference is a
  regression.
- **DPI validation per revision.** Compx encodes DPI as `(value / 50) - 1`;
  Areson has no formula and can only store the discrete values in its table.
  A shared "multiple of 100" rule was wrong in both directions.
- **INI parsing**, including that an inline `;` comment is stripped while
  `color=#ff0000` survives.
- **Action name round-trip.** `--save` turns stored bytes back into names, so
  every name in the tables is pushed through bytes → name → bytes and must come
  back identical. The hazard is aliases: several names share one encoding
  (`none`/`disable`), and `left` is both a mouse button and an arrow keycode —
  emitting `left` for keycode `0x50` would silently turn an arrow binding into a
  click. 174 names, and a new unlisted alias fails the suite.
- **Config round-trip**, the strongest offline check here: build the packets a
  config would be written as, apply them to a simulated device memory, read that
  memory back through the addresses `M913_READ_CODES` actually asks for, decode
  it, and require what comes out to equal what went in — then write it as an INI
  and parse that too. It covers the write path and the read path at once; a
  wrong address on either side breaks it. This is what makes `--save`
  trustworthy without a mouse to hand. It also checks that an erased
  (all-`0xFF`) device decodes to reported gaps rather than invented values.
- **Macros.** The encoding was recovered from the vendor software and then
  confirmed on a real mouse ([MACRO-PROTOCOL.md](MACRO-PROTOCOL.md)), and the
  suite pins the bytes to what that testing established: the count at offset
  `0x1f`, five-byte events from `0x20`, `0x80`/`0x40` for press/release, the
  3 ms delay floor, the trailing checksum over count-plus-events, the 70-event
  cap, modifiers going out as ordinary keys rather than modifier events, and a
  macro's region being written *before* the mapping that points at it. Three of
  those were wrong before hardware corrected them, so they are worth keeping
  pinned.
- **CLI surface**, including that every `--led*` and `--macro` value is rejected
  during option parsing, before the device is opened.

The hardware half detects the mouse in sysfs (so no bus path is hardcoded) and
covers signal handling, driver reattachment, a full config apply, and
configuration read-back. It **rewrites the mouse's stored configuration** and
re-applies `$RESTORE_INI` afterwards — point that at your own config if you keep
one elsewhere.

The read-back check is the one that cannot be done offline. The round-trip above
proves the decoder and the packet builders agree with each other; it cannot prove
the **addresses** are right, since those came from a single captured vendor
session. So the suite writes a config covering every binding shape, reads it back
with `--save`, and requires the values to come back — then feeds the result to
`--config` to confirm it re-applies. On Compx it instead checks that `--save`
refuses, rather than decoding addresses nobody has captured.

Like the ACK count, an empty or incomplete read-back is reported as INFO rather
than failed: the replies come from the mouse, and an idle wireless mouse answers
late. Move the mouse and re-run.

## Interactive hardware verification

```bash
python3 tests/verify-hardware.py
```

`regress.sh` proves the tool builds the right packets. This proves the *mouse*
does the right thing with them, by decoding the HID reports it transmits rather
than trusting what lands in the terminal.

It applies [`tests/verify-hardware.ini`](../tests/verify-hardware.ini), where
every binding exercises a different branch of `build_button_mapping()`, then
walks through seven phases: config apply and ACKs, LED modes, DPI stages,
per-button decoding, multimedia, the fire button, and a final check that the
mouse still works after the listener is stopped.

Expect the cursor to freeze during the button phases — `m913-ctl` claims the
USB interfaces, so the mouse stops driving the pointer. Answer with the
keyboard. The script offers to restore your config at the end.

### Reading the report dumps

Confirmed on `25a7:fa07`:

```
keyboard  EP 0x82   [0]=0x01 report ID   [1]=modifiers   [2..7]=keycodes
consumer  EP 0x82   [0]=0x05 report ID   [1]=usage
mouse     EP 0x81   [0]=button bitmask   [1..]=movement
```

Two traps worth knowing, both of which produced false failures during
development:

- **Byte 0 is a report ID, not a modifier.** Folding it into the modifier mask
  puts a phantom `ctrl` on every binding.
- **Modifier bits and keycodes are different namespaces.** `0x08` is the
  `super` modifier bit *and* the keycode for `e`. Scanning every byte for
  keycodes misreads a modifier-only binding as a letter.

Decode at fixed offsets keyed off the report ID and both disappear.

### Multimedia keys cannot be tested in-band

While `m913-ctl` holds the interface, a consumer report goes to the tool
instead of the OS, so pressing volume-up changes nothing. The script checks
these twice: by decoding usage `0xe9`/`0xcd` from the captured report, and
again in the final phase after the device has been released.

## Things that bite

**Killing the tool can disable the mouse.** `m913-ctl` detaches the kernel HID
driver to claim the interfaces and reattaches on exit. A `SIGKILL` skips that,
leaving the interfaces unbound and the mouse dead. Any later run now reattaches
whatever it finds, so simply running the tool again fixes it:

```bash
m913-ctl --probe        # reattaches the driver as a side effect
```

If the process is gone and the mouse is still unresponsive, replug the
receiver. To inspect the state directly:

```bash
for i in /sys/bus/usb/devices/*:1.*/driver; do echo "$i -> $(readlink "$i")"; done
```

**A packet the mouse never acknowledges did not land.** The acknowledgement is
generated by the *mouse*, not by the receiver dongle, and travels back over the
2.4 GHz link. A mouse sitting still stops answering within seconds, and a write
sent into that silence is simply lost — `HidD_SetFeature` or
`libusb_control_transfer` returning success only means the *host* accepted the
report.

That was learned the hard way, writing macros to a real mouse: a region read
back as `ff ff ff ff` where a packet had reported success. Two things fix it,
and the second matters more than the first.

- **Each packet is re-sent** until acknowledged, up to six times. Re-sending is
  safe for everything this protocol carries — a write puts the same bytes at the
  same address, a read has no side effects, and the vendor software sends the
  commit twice anyway — and it seems to help wake an idle radio.
- **The acknowledgement is matched to the packet** (`ack_matches`). EP `0x82`
  also carries HID input reports and the answers to *earlier* requests, so
  taking whatever arrives next will eventually credit one packet's
  acknowledgement to another. That is exactly how the dropped write above
  reported success. The match is on report ID, sub-command and address — not on
  the whole packet, because a commit's reply carries status bytes rather than
  the payload it was sent.

Anything still unanswered is counted and reported at the end, and the run exits
non-zero: a partial write should not pass for a complete one. `regress.sh` still
reports the ACK count as INFO rather than asserting it, since the result depends
on whether the mouse happened to be in use.

A *short write* throws rather than being retried: a missing acknowledgement
means the outcome is unknown, whereas a truncated `SET_REPORT` means the packet
demonstrably never left.

**Each invocation writes a complete button map.** Buttons not mentioned are
reset to their defaults, so any test that touches one button rewrites all
sixteen. The same is true of the five DPI slots. Both scripts restore a config
at the end for this reason — and `m913-ctl --save before.ini` is worth running
first if the mouse holds a setup you care about.

**The offline harnesses compile against the real sources**, and everything
except `main.cpp` and `usb.cpp` links without libusb. That is deliberate:
`build_config_sequences()` (config → packets) and `decode_device_config()`
(bytes → config) are pure functions, with the USB transfers kept in `main.cpp`.
Moving logic into `main.cpp` puts it beyond the reach of the suite.

## Non-root access

```bash
sudo cp udev/99-m913.rules /usr/lib/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Replug the receiver afterwards. Without this the scripts need `sudo`, and the
sysfs driver checks will still work but the device sections will fail to open.
