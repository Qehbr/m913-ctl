# m913-ctl

Linux configuration tool for the **Redragon M913 Impact Elite** wireless mouse (USB VID `25a7`, PID `fa07`).

Reverse-engineered from USB captures of the official Redragon Windows software. No Windows required.

## Features

- **Button remapping** — all 16 buttons (12 side + left/right/middle/fire)
- **Key combinations** — modifier+key (`ctrl+c`), multi-key (`a+b`, max 3 keys)
- **Multimedia keys** — play, next, prev, stop, volume, mute, email, calculator, browser controls
- **Fire button** — configurable speed and repeat count
- **DPI profiles** — 5 slots, 100–16000 DPI in steps of 100
- **LED modes** — off, steady (color + brightness), respiration (color + speed), rainbow
- **Polling rate** — 125, 250, 500, or 1000 Hz
- **Config files** — INI format for saving and sharing configurations

## Installation

### Pre-built binary

Download the latest `m913-ctl` from [Releases](../../releases/latest), then:

```bash
chmod +x m913-ctl
sudo mv m913-ctl /usr/local/bin/
```

### Build from source

Requires: Linux, libusb 1.0, CMake 3.15+, C++17 compiler (GCC 7+ or Clang 5+).

```bash
sudo apt install libusb-1.0-0-dev cmake build-essential  # Debian/Ubuntu
sudo pacman -S libusb cmake                               # Arch

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cp build/m913-ctl /usr/local/bin/
```

## Setup

Install the udev rule for non-root USB access:

```bash
sudo cp udev/99-m913.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

## Usage

### Command line

```bash
# Remap buttons
m913-ctl --button side1=f1 --button side2=ctrl+c

# Set DPI
m913-ctl --dpi 1=800 --dpi 2=1600 --dpi 3=3200

# Set LED
m913-ctl --led steady
m913-ctl --led respiration
m913-ctl --led rainbow
m913-ctl --led off

# Set polling rate
m913-ctl --polling-rate 1000

# Apply config file
m913-ctl --config examples/example.ini

# List all valid action names
m913-ctl --list-actions
```

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
- `fire` — default fire (hardware auto-repeat)
- `fire:speed:times` — custom speed (3–255, lower=faster) and repeat count (0–3)

### Multimedia
`media_play` `media_player` `media_next` `media_prev` `media_stop`
`media_vol_up` `media_vol_down` `media_mute`
`media_email` `media_calc` `media_computer` `media_home`
`media_search` `www_forward` `www_back` `www_stop` `www_refresh` `www_favorites`

### Keyboard keys
All standard keys: `a`–`z`, `0`–`9`, `f1`–`f24`, `enter`, `space`, `tab`, `backspace`, `esc`, `delete`, `insert`, `home`, `end`, `pageup`, `pagedown`, arrow keys, numpad keys, etc.

### Key combinations
- Modifier + key: `ctrl+c`, `shift+f4`, `alt+f4`, `ctrl+shift+z`
- Multi-key (max 3): `a+b`, `a+b+c`
- Modifiers: `ctrl`, `shift`, `alt`, `super` (or `ctrl_l`, `ctrl_r`, `shift_l`, etc.)

## LED settings

| Parameter | Range | Modes |
|-----------|-------|-------|
| `mode` | `off`, `steady`, `respiration`, `rainbow` | — |
| `color` | Hex RGB (`ff0000` = red) | steady, respiration |
| `brightness` | 0–255 (10 hardware levels) | steady |
| `speed` | 1–5 (1=slowest, 5=fastest) | respiration |

## Diagnostics

```bash
m913-ctl --probe          # show USB interfaces and endpoints
m913-ctl --listen         # listen for mouse packets (Ctrl+C to stop)
m913-ctl --dump           # read config from mouse
m913-ctl --raw-send HEX   # send raw packet for debugging
```

## Acknowledgments

Protocol knowledge derived from [mouse_m908](https://github.com/dokutan/mouse_m908) by dokutan.

## License

GPL-3.0 — see [LICENSE](LICENSE).
