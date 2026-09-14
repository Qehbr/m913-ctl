# M913 macro protocol, from the vendor binaries

Derived by static analysis of the official Redragon M913 software
(`C:\Program Files (x86)\Redragon M913`, `OemDrv.exe` dated 2020-04-20, PE32
x86). No USB capture was needed: the app carries debug logging whose format
strings name every operation, and both macro codecs are compiled straight
through.

**Status: confirmed on hardware** (`25a7:fa07`, wireless). Macros written by
this tool run on the mouse — an autoclicker, a `shift`-modified string, keys
with per-event delays, and all three repeat modes. The device was also read
back after the vendor software had written three macros of its own, and every
field matched what its UI displayed for them.

Static analysis got three things wrong, all corrected below and each marked
**[FIXED]**: the press/release bits were inverted, the two loop modes were
swapped, and the checksum's coverage was too wide. A fourth correction came
only from testing — see §9, on modifiers.

Each point is marked:

- **[CODE]** — read directly from an instruction, table or constant
- **[HW]** — confirmed on the device
- **[INFER]** — reasoned from surrounding context, not individually tested

Anything reimplemented from this must be written from the description, not
transcribed from the binary.

---

## 1. Where the logic lives

| File | Role |
|---|---|
| `Lowerdev.dll` | HID transport only. Nine exports: `FindHidDevice`, `OpenHidDevice`, `SetFeature`, `GetFeature`, `SetOutputReport`, `GetInputReport`, `GetProductString`, `GetProductID`, `GetVersionNumber`. Imports `HID.DLL` and `SETUPAPI`. |
| `OemDrv.exe` | Everything else: packet building, the memory map, the macro codecs, the UI. Loads `Lowerdev.dll` with `LoadLibraryW` and resolves the procs into a device object. |
| `Cfg.ini` | Per-model capability description, in plain text. |
| `Text/*/text.xml` | UI strings, UTF-16. Names every macro feature. |

**[CODE]** `SetFeature` sits at device-object `+0x0c`, `GetFeature` at `+0x10`,
`GetInputReport` at `+0x14`, with the HID handles at `+0x20`/`+0x24`. That the
shim exposes *both* `SetFeature` and `SetOutputReport` independently confirms
the split this project already implements: feature report `0x0308` for Areson,
output report `0x0208` for Compx.

**[CODE]** The packet writer is at `0x4404d0`. It logs
`WriteEEPROM( Addr=0x%x ): `, and takes `(addr, buf, flags)` with the **length
in `ecx`**. A matching `ReadEEPROM` sits near `0x440880`.

---

## 2. Memory map, from every WriteEEPROM call site

**[CODE]** Thirteen call sites, with their literal address and length:

| Address | Len | Holds | Known here before? |
|---|---|---|---|
| `0x0000` | 2 | polling rate | yes |
| `0x0002` | 2 | active DPI stage count | yes |
| `0x000c` | 8 | DPI values | yes |
| `0x002c` | 8 | the block this project calls `unknown_2` | yes, purpose still unnamed |
| `0x004c` | 6 | **DpiRGB** — per-DPI-stage colour | **new** |
| `0x0054` | 8 | LED colour, mode, brightness | yes |
| `0x0058` | 2 | LED mode | yes |
| `0x0060` | 8 | button mapping, two buttons per write | yes |
| `0x00a0` | 7 | **MainRGB** | **new** |
| `0x0100` + n x `0x20` | | per-button keyboard event lists | yes |
| `0x0300` + n x `0x180` | <=384 | **per-button macro** | **new** |

The two RGB regions are named by the app's own log lines
(`DpiRGB, ReadEEPROM(0x4C) = %x`, `MainRGB, ReadEEPROM(0xA0) = %x,...`,
`LogoRGB, ReadEEPROM(0x58) = %x`). So this hardware has three lighting zones,
of which m913-ctl currently drives one.

---

## 3. Macro storage

**[CODE]** A table of 16 dwords at `0x5cb1e0`, indexed by button index, gives
each button's macro address:

```
0x0300, 0x0480, 0x0600, 0x0780, 0x0900, 0x0a80, 0x0c00, 0x0d80,
0x0f00, 0x1080, 0x1200, 0x1380, 0x1500, 0x1680, 0x1800, 0x1980
```

That is `0x0300 + n * 0x180` — 16 regions of 384 bytes. **One macro per button,
not a shared pool of macro slots.** A button's action does not name a macro;
the mouse runs the macro belonging to that button. This is why `--get` finds
exactly 16 erased regions: one per button, the same shape as the keyboard event
lists at `0x0100`.

Note the base is `0x0300`, not the `0x0301` this project's read codes probe —
the captured vendor session happened to read from +1.

**[CODE]** Limits, initialised together at `0x43c8b3`:

| Global | Value | Meaning |
|---|---|---|
| `0x5df028` | `0x180` | region size in bytes (384) |
| `0x5df02c` | `0x10` | number of macro regions (16) |
| `0x5df030` | `0xff` | maximum repeat count |
| `0x5df038` | `0x46` | **maximum events per macro (70)** |

70 events x 5 bytes = 350, plus a 32-byte header and a checksum byte: 383 of
the 384. See section 7 — the cap is what pins the events to offset 0x20.

---

## 4. Macro byte format

**[CODE]** From the decoder `HdMacro_To_StMacro` (`0x40a710`) and the encoder
`StMacro_To_HdMacro` (`0x40a980`):

```
offset 0x1f   event count (1..70)          see section 7 for the offset
offset 0x20+  5 bytes per event:
  +0          (action << 6) | kind
  +1          key code
  +2          always 0x00
  +3          delay, high byte
  +4          delay, low byte
after the last event: one checksum byte, then the region is zero-filled
```

- `action` is the top two bits: **`0x80` = key DOWN, `0x40` = key UP**
  **[HW, FIXED]**. The disassembly suggested the opposite; the device settled
  it. Every event the vendor UI showed as a key-down reads `0x8n` on the mouse,
  every key-up `0x4n`. Anything else is rejected with `convert err`.
- `kind` is the low three bits (`and esi, 7`) — all three tables are extracted
  in section 7, and none of them needs a translation layer:
  - `0` — modifier; the byte is the HID modifier bitmask (works, but do not
    use it — see section 9)
  - `1` — regular key; the byte is the HID usage code
  - `4` — mouse button; the byte is the mouse bitmask
  - other values are rejected with `convert err, nKeyFalg=%d`
- `delay` is 16-bit **big-endian milliseconds**. The decoder maps a stored value
  of 3 to 0 **[INFER: 3 ms is the floor the firmware honours]**. `Cfg.ini` sets
  `MinDelay=0`.

`MacroHasMsKey=0x07` in `Cfg.ini` is a three-bit mask matching the three mouse
buttons the UI offers **[INFER]**, consistent with kind `4` accepting more codes
than the UI exposes.

---

## 5. Pointing a button at its macro

**[CODE]** The per-button action dword is built as

```
action = 0x06 | (button_index << 8) | (repeat << 16)
```

so the four bytes in the mapping block at `0x0060 + index*4` are

```
[0] = 0x06          "run this button's macro"
[1] = button index
[2] = repeat
[3] = checksum, (0x55 - sum of the first three) & 0xFF
```

The type code comes from a dispatcher at `0x40a1f0` that returns packed action
bytes for every binding kind. Its other returns confirm codes this project
already implements: `0x00000101` left, `0x00000201` right, `0x00000401` middle,
`0x00000801` back, `0x00001001` forward, and `0x00000005` for the
keyboard-event-list binding — which is exactly the `05 00 00 50` marker in
`data.h`, since `0x55 - 5 = 0x50`. **Macro is the code immediately next to one
we already use and trust, which is strong corroboration.**

**[CODE]** `repeat` comes from the macro's mode field:

| `repeat` byte | Meaning | |
|---|---|---|
| `0x01`-`0xfd` | run the macro that many times | **[HW]** |
| `0xfe` | **cycle until the key is released** — repeat while held | **[HW, FIXED]** |
| `0xff` | **cycle until any key is pressed** — a toggle | **[HW, FIXED]** |

The two loop modes are the other way round from what the disassembly implied:
the vendor UI's radio-button order is not the order of the mode enum behind it.
Settled by pointing the vendor software at three macros — "until released",
"until any key" and "3 times" — and reading back `0xfe`, `0xff` and `0x03`.

**This answers the autoclicker question.** A two-event macro (left down, left
up) with `repeat = 0xff` is a hardware repeat-while-held, and `0xfe` is a
toggle. The fire button's three-click ceiling was never the real limit — it is
just a weaker, separate feature.

---

## 6. What this also confirms about existing work

- `Text/en/text.xml` contains `Times ( 0-3 )` for the fire button: the vendor UI
  agrees with the 0-3 range measured on hardware for `fire:speed:times`.
- `0x40a1f0` returns `0x00033204` for one binding — bytes `04 32 03`, exactly
  this project's `three_click`. So `three_click` and `fire:50:3` are the same
  thing in the firmware, as `action_name()` already documents.
- `Cfg.ini`'s `K1_1`..`K16_1` entries end in a permutation of 1..16 mapping the
  twelve side buttons to indices 0-5 and 8-15, right to 6, left to 7, middle to
  10 and fire to 11 — identical to the `Button` enum in `protocol.h`, derived
  independently from a different source.
- `Cfg.ini` describes **two sensors**: `0x3325` with DPI stages topping out at
  10000, and `0x3335` reaching 16000. This install is configured for `0x3325`.
  Worth checking whether the 11000-16000 entries in `dpi_table` are reachable on
  every unit.

---

## 7. The codes, resolved

**[CODE]** All three code tables extracted. There is no vendor-specific
numbering to translate — the hardware stores the same codes this project
already uses everywhere else.

**kind 1 — regular key.** `byte[1]` is a **USB HID keyboard usage code**. The
converter at `0x438ae0` is a 104-entry linear search over a table of
`(hid, vk)` byte pairs at `0x5a06a0`, purely so the UI can show a Windows
virtual-key name. The HID half is identical to `key_codes` in `data.cpp`:
`0x04`→A … `0x1d`→Z, `0x1e`→1 … `0x27`→0, `0x28` enter, `0x29` esc,
`0x2a` backspace, `0x2b` tab, `0x2c` space, `0x3a`–`0x45` F1–F12,
`0x4f`–`0x52` arrows, `0x53`–`0x63` numpad, `0x65` menu, `0xe0`–`0xe7`
the modifiers.

**kind 0 — modifier.** `byte[1]` is the **HID modifier bitmask**, one bit only,
via the table at `0x40a934`/`0x40a918`:

| byte | modifier |
|---|---|
| `0x01` | left ctrl |
| `0x02` | left shift |
| `0x04` | left alt |
| `0x10` | right ctrl |
| `0x20` | right shift |
| `0x40` | right alt |

`0x08` and `0x80` — left and right **super/GUI — are rejected** by the decoder,
so a macro cannot hold the Windows key as a modifier. **[INFER]** It may still
be expressible as kind 1 with HID usage `0xe3`/`0xe7`, which the `0x5a06a0`
table does map; untested.

**kind 4 — mouse button.** `byte[1]` is the **mouse bitmask** already used by
this protocol's `0x01` actions, via `0x40a908`/`0x40a8f0`:

| byte | button | internal code |
|---|---|---|
| `0x01` | left | `0xf0` |
| `0x02` | right | `0xf1` |
| `0x04` | middle | `0xf2` |
| `0x08` | backward | `0xf4` |
| `0x10` | forward | `0xf3` |

Any other value falls through to the `convert err` path.

### Event bytes, from the encoder

**[CODE]** `StMacro_To_HdMacro` composes each event as:

```
byte 0 = kind | 0x40   for a press      (so action 1 = down)
         kind | 0x80   for a release    (action 2 = up)
byte 1 = code
byte 2 = 0x00          always; written unconditionally, never read back
byte 3 = delay >> 8
byte 4 = delay & 0xFF
```

**[CODE]** The delay is **clamped up to a minimum of 3 ms** by the encoder,
which is why the decoder maps a stored 3 back to 0: 3 ms is the floor the
firmware honours, and the UI shows it as "no delay".

### Where the events sit in the region

**[INFER, strongly]** Events start at **offset `0x20`**, and the event count is
the byte at **offset `0x1f`**. Three independent things agree:

- the encoder initialises its output offset to `0x20` and steps 5;
- it stores the count one byte below the first event;
- and the 70-event cap is exactly `(0x180 - 0x20) / 5 = 70.4`, i.e. 70 events
  is what remains of a 384-byte region after a 32-byte header. A layout with
  the count at offset 0 would fit 76, not 70.

Bytes `0x00`–`0x1e` are zero-filled by the encoder and never read back; their
purpose is unknown. **This offset is the single most important thing for
hardware confirmation to check**: if a macro does not fire, try the count at
offset `0` with events from offset `1`.

### Checksum and packet chunking

**[CODE]** The checksum is the same as everywhere else in this protocol:
`(0x55 - sum(bytes)) & 0xFF`. The writer's two accumulators — even-offset bytes
into one register, odd into another, with a third pickup for a trailing odd
byte — are just an unrolled sum, not a different formula. It is stored
immediately after the last event, at `0x20 + 5*count`, and covers every byte
before it.

**[CODE]** `WriteEEPROM`'s fourth argument is the payload chunk size, and it
defaults to **10** when zero (`mov ebx,0xa`). The button-mapping, DPI and
`unknown_2` writes all pass 8 explicitly — which is why every template in this
project uses `len=8` with the address stepping by 8 — but the **macro write
passes 0, so macro packets carry 10 payload bytes** and the address steps by
10. The whole 384-byte region is written each time, zero-filled past the macro.

## 8. Hardware confirmation

Everything above was exercised on `25a7:fa07` over the wireless receiver, by
writing packets this tool generated and then pressing the button. Confirmed
working:

- an autoclicker — `click left 50` with repeat `0xfe`, repeating while held
- a typed string with a held modifier — `Hello`, 12 events
- per-event delays, including the 3 ms floor
- all three repeat modes: a literal count, repeat-while-held, and the toggle
- macros on five different buttons, which exercises the `0x180` region stride
- reading a macro back off the device and decoding it field by field

The read-back path was validated the same way: polling rate, active stage count
and all five DPI slots decoded off the device exactly as this tool encodes
them — `01`=1000 Hz, `05` stages, `04 04 00 4d`=400 dpi and so on.

The one thing static analysis could not have told us is in §9.

## 9. Modifiers: send them as keys, not as modifier events

**[HW]** This is the one thing no amount of reading the binary would have
found, and it is the difference between macros working and silently doing
nothing.

A modifier can be expressed two ways, and the decoder tables accept both:

- as a **kind 0** event, whose byte is the HID modifier bitmask (`0x02` = left
  shift), or
- as an ordinary **kind 1** key, using the modifier's HID *usage* code
  (`0xe1` = left shift), which the `0x5a06a0` table maps for exactly this.

Both work in a short macro. But a kind 0 event costs the firmware something
extra at run time, and past roughly ten events it stops working altogether —
silently, with no partial output:

| Macro | Events | Modifier form | Result |
|---|---|---|---|
| `click left 50`, hold | 2 | none | runs |
| `shift+a`, hold | 4 | kind 0 | runs, types `A` |
| `shift+a`, hold | 4 | kind 1 (`0xe1`) | runs, types `A` |
| `A` then `b` | 6 | kind 0 | runs, types `Ab` |
| `Hell` | 10 | kind 0 | runs |
| `hello` | 10 | none | runs |
| `a` ×6 | 12 | none | runs |
| vendor's `qwerty` | 12 | none | runs |
| **`Hello`** | **12** | **kind 0** | **nothing at all** |
| **`Hello`** | **12** | **kind 1 (`0xe1`)** | **runs, types `Hello`** |

The last two rows are the same twelve events, the same delays and the same
button, differing only in how `shift` is encoded. Note also that not one of the
three macros the vendor software wrote to the device contained a kind 0 event —
it uses keys for modifiers too.

So `parse_macro_spec()` always emits modifiers as keys, mapping the bitmask to
its usage code (`0x01`→`0xe0` … `0x80`→`0xe7`). Two things follow:

- there is no practical modifier-related length limit, and
- **`super`/meta becomes usable**, which kind 0 cannot express at all — the
  decoder rejects bits `0x08` and `0x80`, but usages `0xe3`/`0xe7` are in the
  table. (Accepted by the tool and written correctly; not separately tested by
  pressing a button.)

Where exactly the kind 0 ceiling sits, and why, is unknown — somewhere between
10 and 12 events. Since nothing needs kind 0, it was not worth narrowing
further. `MacroKind::Modifier` remains defined for completeness.

## 10. Writing macros over a wireless link

**[HW]** Two practical findings, both of which cost time to discover:

- **A write can be silently dropped.** `HidD_SetFeature` succeeding, or a
  `libusb` control transfer succeeding, means the host accepted the report —
  not that the mouse applied it. A dropped packet in the middle of a macro
  leaves the region well-formed but wrong, and the macro then does nothing.
- **The ACK must be matched to the request.** The device echoes each packet
  back with report ID `0x09` and the checksum one lower (`0x4C` base instead of
  `0x55`). Accepting *any* reply lets a lagging ACK from an earlier packet mask
  a dropped write — which is exactly how one packet went missing during
  testing. Match on sub-command and address, then retry until that packet's own
  ACK arrives.

An idle 2.4 GHz mouse also stops answering entirely within seconds, so a long
write needs either the cable or a moving mouse. `send_cmd()` in `main.cpp`
currently accepts any reply and does not retry; that is worth changing.

## 11. What the capture plan is still for

`CAPTURING-MACROS.md` is superseded for Areson. It remains the only route for
**Compx** (`3554:f55d`), whose addressing differs and which this install's
software does not cover — that variant ships with different vendor software, and
the same static-analysis approach should be tried on it first.
