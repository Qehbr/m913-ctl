# m913-ctl

Linux configuration tool for the **Redragon M913 Impact Elite** wireless mouse.

Reverse-engineered from USB captures of the official Redragon Windows software. No Windows required.

## Supported hardware

Two hardware revisions of the M913 ship under the same name. Both are supported and auto-detected:

| Revision | Wireless | Wired |
|----------|----------|-------|
| Original (Areson) | `25a7:fa07` | `25a7:fa08` |
| Newer (Compx, tri-mode) | `3554:f55d` | `3554:f55e` |

The Compx revision differs in a few ways — see [Compx hardware notes](#compx-hardware-notes) below.

## Features

- **Button remapping** — all 16 buttons (12 side + left/right/middle/fire)
- **Key combinations** — modifier+key (`ctrl+c`), multi-key (`a+b`, max 3 keys)
- **Multimedia keys** — play, next, prev, stop, volume, mute, email, calculator, browser controls
- **Fire button** — configurable speed and repeat count
- **DPI profiles** — 5 slots (Areson: fixed set up to 16000; Compx: any multiple of 50 up to 12750)
- **LED** — Areson: off/steady/respiration/rainbow modes; Compx: per-DPI-stage RGB color
- **Polling rate** — 125, 250, 500, or 1000 Hz
- **Config files** — INI format for saving and sharing configurations
- **Read-back** — `--save` decodes the mouse's stored configuration into an INI
  file you can edit and re-apply (Areson)

## GUI

If you prefer a visual interface over the command line, you can use [m913-ctl-gui](https://github.com/brunofin/m913-ctl-gui) by **brunofin**. 

> **Note:** This is a graphical wrapper and **requires `m913-ctl` to be installed** from this repository first. The GUI uses this CLI tool as its backend to communicate with the mouse.

## Installation

### Arch Linux (AUR)

```bash
yay -S m913-ctl
# or: paru -S m913-ctl
```

The udev rule is installed automatically. After installation, **replug the mouse receiver** (or run `sudo udevadm trigger`) for the rule to take effect.

### Pre-built binary

Download the latest `m913-ctl` from [Releases](../../releases/latest), then:

```bash
chmod +x m913-ctl
sudo mv m913-ctl /usr/local/bin/
```

Also download `99-m913.rules` from the same release and install it for non-root USB access:

```bash
sudo mv 99-m913.rules /usr/lib/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

### Build from source

Requires: Linux, libusb 1.0, CMake 3.15+, C++17 compiler (GCC 7+ or Clang 5+).

```bash
sudo apt install libusb-1.0-0-dev cmake build-essential  # Debian/Ubuntu
sudo pacman -S libusb cmake                               # Arch

cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

The version reported by `--version` comes from the nearest git tag. When
building from a release tarball there is no git history, so pass it explicitly
— distro packaging should always do this, otherwise the binary reports `dev`:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DAPP_VERSION=1.0.9
```

## Usage

### Command line

```bash
# Remap buttons — all remapped buttons must be in one command
m913-ctl --button side1=ctrl+1 --button side4=ctrl+4

# Set DPI
m913-ctl --dpi 1=800 --dpi 2=1600 --dpi 3=3200

# Set LED
m913-ctl --led steady --led-color ff0000 --led-brightness 200
m913-ctl --led respiration --led-speed 4
m913-ctl --led rainbow
m913-ctl --led off

# Set polling rate
m913-ctl --polling-rate 1000

# Apply config file
m913-ctl --config examples/example.ini

# Read the mouse's current configuration back as an INI file
m913-ctl --save my-setup.ini
m913-ctl --save > my-setup.ini      # or to stdout

# Config file plus overrides — the flags win, and it is all sent as one write
m913-ctl --config my-setup.ini --dpi 1=1600 --led off

# List all valid action names
m913-ctl --list-actions
```

> **Note:** `--button` and `--dpi` each write a **complete block**. Any button
> or DPI slot you do not mention is reset to its factory default — the mouse
> stores all 16 buttons as one block and all 5 DPI slots as another, and there
> is no way to change one entry in isolation. So pass everything you want in a
> single command, or keep it in a config file. `--save` writes one out for you.

> **Note:** A config file and inline flags can be combined. They are merged
> into one configuration and written once, with the flags overriding the file
> — so `--config my.ini --dpi 1=1600` keeps everything in the file and changes
> only DPI slot 1.

> **`(no ACK within 1.5s)`?** The acknowledgement comes from the mouse itself over
> the wireless link, and an idle mouse throttles its radio, so replies often
> arrive too late. The settings still apply. Keep the mouse moving while the
> command runs and the ACKs come back.

### Config file

```ini
[mouse]
polling_rate=1000

[dpi]
dpi1=400
dpi2=800
dpi3=1600
dpi4=3200
dpi5=6400

[led]
mode=steady
color=ff0000
brightness=255
speed=3

[buttons]
button_left=left
button_right=right
button_middle=middle
button_fire=fire:58:3
button_side1=ctrl+c
button_side2=ctrl+v
button_side3=media_play
button_side4=www_back
```

See [examples/example.ini](examples/example.ini) for a complete example.

### Reading your configuration back

`--save` reads the configuration out of the mouse and writes it as an INI file
that `--config` accepts:

```bash
m913-ctl --save my-setup.ini
```

This is the way to take over a setup made with the vendor software on Windows,
or to recover one you no longer have the file for. All 16 buttons, the 5 DPI
slots, the active stage count, the LED state and the polling rate come back.

- **Areson only.** The Compx revision answers a different report type at
  addresses that have never been captured, so there is nothing reliable to
  decode there; `--save` refuses rather than guess, and `--get` shows the raw
  replies. Fixing that needs USB captures of the Windows software talking to a
  Compx device.
- **Keep the mouse moving while it runs.** The replies come from the mouse
  itself over the 2.4 GHz link, not from the receiver, so an idle wireless
  mouse answers late. Each block gets a second attempt, and anything that never
  answers is reported both on stderr and as a comment in the file.
- A binding the decoder cannot name is written as a commented-out line with its
  raw bytes, so the file stays applicable and still records what was there.
  That is worth reporting as a bug — it means an action the mouse stores has no
  name in this tool.

## Button names

| Name | Physical button |
|------|----------------|
| `left` | Left click |
| `right` | Right click |
| `middle` | Scroll wheel click |
| `fire` | Fire button (near left click) |
| `side1`–`side12` | 12 side buttons |

## Action reference

### Mouse actions
`left` `right` `middle` `forward` `backward`

### DPI controls
`dpi+` `dpi-` `dpi-cycle`

### Special
`led_toggle` `three_click` `polling_switch` `none`

### Fire button

Rapid fire / burst click — one press of the fire button sends a short burst of
left clicks.

- `fire` — default burst (speed 58, 3 clicks)
- `fire:speed:times` — `speed` 3–255 (lower = faster), `times` = clicks per press, **0–3**

> **This is not an autoclicker.** `times` is a fixed number of clicks per press,
> not a repeat-while-held mode, and **3 is a hardware ceiling**, not a
> conservative choice. Measured on `25a7:fa07`:
>
> | value written to the mouse | 0 | 1 | 2 | 3 | 4 or more |
> |---|---|---|---|---|---|
> | clicks actually fired | 1 | 1 | 2 | 3 | **0** |
>
> The firmware has no encoding for "no clicks" below 4, and treats 0 as 1. So
> `m913-ctl` writes 4 when you ask for `times=0`, which is the value that really
> fires nothing — `times` therefore means exactly what it says across 0–3.
>
> Do not raise the limit in `parse_action()` without re-testing on hardware: the
> mouse stores any value you give it and then silently refuses to honour it,
> so a raised cap looks like it worked and produces a dead button.

> **Note:** The minimum usable speed depends on your OS debounce threshold. Very low values (e.g. `speed=3`) may cause all clicks to register as one. Start around `speed=25` and tune down until clicks stop being detected.

### Multimedia
`media_play` `media_player` `media_next` `media_prev` `media_stop`
`media_vol_up` `media_vol_down` `media_mute`
`media_email` `media_calc` `media_computer` `media_home`
`media_search` `www_forward` `www_back` `www_stop` `www_refresh` `www_favorites`

### Keyboard keys
All standard keys: `a`–`z`, `0`–`9`, `f1`–`f24`, `enter`, `space`, `tab`, `backspace`, `esc`, `delete`, `insert`, `home`, `end`, `pageup`, `pagedown`, arrow keys, numpad keys, etc.

> **Arrow keys:** `left` and `right` are mouse-button names, so on their own they
> bind a mouse click. Use `arrow_left` / `arrow_right` (and `arrow_up` /
> `arrow_down`) to bind the arrow keys. Inside a combo the plain names work
> fine — `ctrl+left` is the arrow key.

### Key combinations

A binding may combine **at most 3 modifiers and keys in total**. The mouse
stores each binding as a fixed-size list of HID events, and three is what
fits — so `ctrl+shift+z` and `a+b+c` are fine, but `ctrl+shift+alt+f4` (four)
is rejected with an error.

- Modifier + key: `ctrl+c`, `shift+f4`, `alt+f4`, `ctrl+shift+z`
- Multi-key: `a+b`, `a+b+c`
- Modifiers: `ctrl`, `shift`, `alt`, `super` (or `ctrl_l`, `ctrl_r`, `shift_l`, etc.)

## LED settings

These apply to the **original (Areson)** hardware. For the Compx revision, see below.

| Parameter | Range | Modes |
|-----------|-------|-------|
| `mode` | `off`, `steady`, `respiration`, `rainbow` | — |
| `color` | Hex RGB (`ff0000` = red) | steady, respiration |
| `brightness` | 0–255 (10 hardware levels) | steady |
| `speed` | 1–5 (1=slowest, 5=fastest) | respiration |

Each has a command-line equivalent: `--led`, `--led-color`, `--led-brightness`,
`--led-speed`.

## Compx hardware notes

The newer Compx revision (`3554:f55d` / `3554:f55e`) is auto-detected and uses the
same commands, with a few differences:

- **DPI** is any multiple of 50 from 50 to 12750 (e.g. `400`, `450`, `500`).
  Areson instead has a fixed table of supported values reaching 16000 — it is
  not every multiple of 100, and an unsupported value is now rejected with the
  nearest supported one rather than silently ignored.
- **LED is per DPI stage** — there are no global modes. Each DPI stage has its own
  RGB color, set with `dpiN_color` in the `[dpi]` section. `000000` turns that
  stage's LED off. The `[led]` section, if present, applies one color (or `off`)
  to every active stage; per-stage `dpiN_color` overrides it.

```ini
[dpi]
dpi1=400
dpi1_color=ff0000
dpi2=800
dpi2_color=0000ff
dpi2_enable=0      ; collapse to a single active stage (top-down only)
```

See [examples/example_compx.ini](examples/example_compx.ini) for a complete example.

## Diagnostics

```bash
m913-ctl --probe           # show USB interfaces and endpoints
m913-ctl --listen          # listen on both endpoints (Ctrl+C to stop)
m913-ctl --listen 0x82     # listen on one endpoint only
m913-ctl --probe-commands  # probe which command bytes the device answers
m913-ctl --raw-send HEX    # send raw packet for debugging
m913-ctl --get             # raw form of --save: print replies as hex
m913-ctl --get 12          # read one block (0-68)
```

`--get` is `--save` without the decoding, for protocol work. It also walks the
16 regions of 384 bytes at `0x0301`+ that `--save` skips: they read as erased
flash and nothing is known to live there.

## Development

Build, then run the regression suite:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./tests/regress.sh                  # offline checks + device checks
OFFLINE_ONLY=1 ./tests/regress.sh   # never opens the mouse
```

For changes that affect what the mouse actually transmits, there is an
interactive check that decodes the device's own HID reports:

```bash
python3 tests/verify-hardware.py
```

Both **rewrite the mouse's stored configuration** and restore
`examples/example.ini` at the end — run `m913-ctl --save before.ini` first if
the mouse holds a setup you care about. See [docs/TESTING.md](docs/TESTING.md)
for what they cover, how to read the report dumps, and how to recover a mouse
whose kernel driver was left detached.

### Macros: not supported yet

The mouse has 16 regions of 384 bytes at `0x0301` that read as erased flash, and
the vendor software can program macros, but neither the storage format nor the
button action code that points at a macro is known. Both have to come from USB
captures of the Windows software.

If you have a Windows machine with the vendor software and can record USB
traffic (USBPcap + Wireshark) while it programs a macro, that would unblock the
feature — please open an issue. The two captures that matter most are the same
button assigned to a plain key and then to a macro, which isolates the action
code, and a macro of three left clicks, which shows the storage format.

[tools/decode-capture.py](tools/decode-capture.py) does the analysis, and is
useful for any protocol work here: it decodes a capture — or a `--get` dump —
into addressed, checksum-verified writes, names the memory region each one
lands in, and `--diff` shows which bytes two sessions differ by, which is how
an unknown field gets isolated.

```bash
python3 tools/decode-capture.py --diff before.pcapng after.pcapng

m913-ctl --get > before.txt      # or diff memory read off the mouse
m913-ctl --get > after.txt
python3 tools/decode-capture.py --diff before.txt after.txt
```

## Acknowledgments

Protocol knowledge derived from [mouse_m908](https://github.com/dokutan/mouse_m908) by dokutan.

## License

GPL-3.0 — see [LICENSE](LICENSE).
