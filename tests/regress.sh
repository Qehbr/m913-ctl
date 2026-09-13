#!/usr/bin/env bash
#
# Automated regression suite for m913-ctl.
#
# Two halves:
#   * offline  — packet building, validation, config parsing, CLI surface.
#                No device needed; safe to run anywhere, including CI.
#   * hardware — signal handling, driver reattachment, a real config apply.
#                Skipped automatically when no supported mouse is present.
#
# The hardware half REWRITES the mouse's stored configuration. It re-applies
# $RESTORE_INI at the end, so point that at your own config if you keep one
# somewhere other than examples/example.ini.
#
#   ./tests/regress.sh                  # everything available
#   OFFLINE_ONLY=1 ./tests/regress.sh   # never touch the device
#   RESTORE_INI=~/my.ini ./tests/regress.sh
#
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-$REPO/build}"
CTL="$BUILD/m913-ctl"
RESTORE_INI="${RESTORE_INI:-$REPO/examples/example.ini}"
OFFLINE_ONLY="${OFFLINE_ONLY:-0}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

INC="-I $REPO/src $(pkg-config --cflags libusb-1.0 2>/dev/null)"
SRC="$REPO/src/protocol.cpp $REPO/src/data.cpp"
# Everything except main.cpp and usb.cpp, i.e. the whole write and read path
# with no I/O in it. readback.cpp needs libusb's header for UsbMouse but not
# the library, since the harnesses below never open a device.
SRC_ALL="$SRC $REPO/src/config.cpp $REPO/src/readback.cpp"

pass=0; fail=0; skip=0
ok(){   printf "  \033[32mPASS\033[0m  %s\n" "$1"; pass=$((pass+1)); }
no(){   printf "  \033[31mFAIL\033[0m  %s\n" "$1"; fail=$((fail+1)); }
sk(){   printf "  \033[33mSKIP\033[0m  %s\n" "$1"; skip=$((skip+1)); }
chk(){ if [[ "$3" == *"$2"* ]]; then ok "$1"; else no "$1 — got: $(echo "$3" | head -1)"; fi; }
hdr(){ printf "\n\033[1;36m%s\033[0m\n" "$1"; }

# Locate the mouse in sysfs so the driver checks do not hardcode a bus path.
find_usb_dev(){
  local d v
  for d in /sys/bus/usb/devices/*; do
    [[ -f "$d/idVendor" ]] || continue
    v="$(cat "$d/idVendor")"
    if [[ "$v" == "25a7" || "$v" == "3554" ]]; then
      case "$(cat "$d/idProduct")" in
        fa07|fa08|f55d|f55e) basename "$d"; return 0 ;;
      esac
    fi
  done
  return 1
}

# ---------------------------------------------------------------- build
hdr "BUILD"
cmake -B "$BUILD" -S "$REPO" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
W=$(cmake --build "$BUILD" 2>&1 | grep -icE "warning|error")
[[ "$W" == "0" ]] && ok "builds clean (0 warnings under -Wall -Wextra)" || no "$W warnings/errors"
[[ -x "$CTL" ]] || { no "no binary produced"; exit 1; }
ok "binary produced"

# ---------------------------------------------------------------- offline
hdr "Key-combo capacity (ASAN + libstdc++ assertions)"
cat > "$TMP/combo.cpp" <<'EOF'
#include "protocol.h"
#include "data.h"
#include <cstdio>
int main(int, char** v) {
    ActionBytes ab;
    if (!parse_action(v[1], ab)) { printf("PARSEFAIL\n"); return 0; }
    std::map<uint8_t, ActionBytes> m; m[0] = ab;
    if (ab[0] == 0x90 && ab[3] > 1) register_multikey_action(0, v[1]);
    try { printf("OK %zu\n", build_button_mapping(m, nullptr).size()); }
    catch (const std::exception&) { printf("REJECT\n"); }
    return 0;
}
EOF
if g++ -std=c++17 -fsanitize=address -D_GLIBCXX_ASSERTIONS -g $INC \
       "$TMP/combo.cpp" $SRC -o "$TMP/combo" 2>/dev/null; then
  for a in a ctrl+c ctrl+shift+z a+b+c ctrl+a+b super alt+f4; do
    chk "accepted: $a" "OK" "$("$TMP/combo" "$a" 2>&1)"
  done
  # >3 modifiers+keys overflows the two 17-byte sub-packets; must be refused.
  for a in ctrl+shift+alt+f4 ctrl+shift+a+b a+b+c+d; do
    chk "rejected: $a" "REJECT" "$("$TMP/combo" "$a" 2>&1)"
  done
  chk "'left' stays the mouse button" "OK 8" "$("$TMP/combo" left 2>&1)"
  chk "'ctrl+left' is the arrow key"  "OK 10" "$("$TMP/combo" ctrl+left 2>&1)"
  for a in arrow_left arrow_right arrow_up arrow_down; do
    chk "alias $a binds a key" "OK 9" "$("$TMP/combo" "$a" 2>&1)"
  done
else
  sk "ASAN build unavailable — combo capacity checks skipped"
fi

hdr "Compx DPI stage count"
cat > "$TMP/stage.cpp" <<'EOF'
#include "protocol.h"
#include <cstdio>
// The hardcoded cascade this was refactored from, kept as the oracle.
static void orig(const std::array<bool,5>& e, uint8_t& c, uint8_t& p) {
    c = 5; p = 0x50;
    if (!e[4]) { c = 4; p = 0x51; } if (!e[3]) { c = 3; p = 0x52; }
    if (!e[2]) { c = 2; p = 0x53; } if (!e[1]) { c = 1; p = 0x54; }
}
int main() {
    int diff = 0;
    for (int m = 0; m < 32; ++m) {
        DpiSettings s; s.values[0] = 400;
        for (int i = 0; i < DPI_SLOTS; ++i) s.enabled[i] = (m >> i) & 1;
        uint8_t c, p; orig(s.enabled, c, p);
        auto pk = build_compx_dpi_packets(s);
        if (pk.back()[6] != c || pk.back()[7] != p) ++diff;
    }
    printf("STAGEDIFF %d\n", diff);
    DpiSettings a; a.enabled[1] = false;
    printf("ENABLEONLY %zu\n", build_compx_dpi_packets(a).size());
    uint32_t col[DPI_SLOTS] = {1,2,3,4,5};
    printf("COLOR %zu\n", build_compx_color_packets(col, DPI_SLOTS).size());
    printf("SLOTS %d\n", DPI_SLOTS);
    return 0;
}
EOF
g++ -std=c++17 $INC "$TMP/stage.cpp" $SRC -o "$TMP/stage" 2>/dev/null
OUT="$("$TMP/stage")"
chk "stage byte unchanged across all 32 enabled[] patterns" "STAGEDIFF 0" "$OUT"
chk "enable-only config still emits the stage packet"       "ENABLEONLY 1" "$OUT"
chk "colour builder honours DPI_SLOTS"                      "COLOR 5"      "$OUT"
chk "DPI_SLOTS == 5"                                        "SLOTS 5"      "$OUT"

hdr "Compx write path (config → sequences)"
# build_config_sequences() decides what a Config turns into for both hardware
# revisions. The Compx half cannot be checked by the round-trip below — there
# is no read-back for it — so its shape is asserted directly here: which
# sequences get sent, and how many packets each carries.
cat > "$TMP/compxseq.cpp" <<'EOF'
#include "config.h"
#include <cstdio>
static void show(const char* tag, const Config& c) {
    printf("%s:", tag);
    for (auto& s : build_config_sequences(c, COMPX_LAYOUT, true))
        printf(" %s=%zu", s.label.c_str(), s.packets.size());
    printf("\n");
}
int main() {
    Config a;                       // --led off only: colours every stage
    a.led.set = true; a.led.mode = LedMode::Off;
    show("LEDONLY", a);

    Config b;                       // dpi2_enable=0 with no values at all
    b.dpi[1].enabled = false;
    show("ENABLEONLY", b);

    Config c;                       // values + per-stage colours, 3 stages
    for (int i = 0; i < DPI_SLOTS; ++i) { c.dpi[i].value = 800; c.dpi[i].color = 0x10203040 & 0xFFFFFF; }
    c.dpi[3].enabled = false;
    show("FULL", c);
    return 0;
}
EOF
g++ -std=c++17 $INC "$TMP/compxseq.cpp" $SRC_ALL -o "$TMP/compxseq" 2>/dev/null
CS="$("$TMP/compxseq")"
chk "--led off alone colours all 5 stages"        "LEDONLY: LED color=5"          "$CS"
chk "an enable-only config sends just the stage packet" "ENABLEONLY: DPI config=1" "$CS"
chk "values+colours: 5 DPI + stage packet, 3 active stages coloured" \
    "FULL: DPI config=6 LED color=3" "$CS"

hdr "DPI validation is per-revision"
cat > "$TMP/dpi.cpp" <<'EOF'
#include "protocol.h"
#include <cstdio>
int main() {
    int v[] = {50,100,400,450,3100,3200,12750,12800,16000};
    for (int x : v)
        printf("%d:%c%c ", x, dpi_value_supported(x,false)?'A':'-',
                              dpi_value_supported(x,true )?'C':'-');
    printf("\n"); return 0;
}
EOF
g++ -std=c++17 $INC "$TMP/dpi.cpp" $SRC -o "$TMP/dpi" 2>/dev/null
DV="$("$TMP/dpi")"
chk "Compx takes 450, Areson does not"        "450:-C"   "$DV"
chk "Areson refuses 3100 (no table entry)"    "3100:-C"  "$DV"
chk "Areson takes 3200"                       "3200:AC"  "$DV"
chk "Compx takes 12750"                       "12750:-C" "$DV"
chk "Compx refuses 16000 (would truncate)"    "16000:A-" "$DV"

hdr "INI parser"
printf '[dpi]\ndpi1=400\ndpi2=800\ndpi2_enable=0   ; collapse to one stage\n' > "$TMP/a.ini"
printf '[dpi]\ndpi1=400\n[led]\ncolor=#ff0000\n' > "$TMP/b.ini"
cat > "$TMP/cfg.cpp" <<'EOF'
#include "config.h"
#include <cstdio>
int main(int, char** v) {
    try {
        Config c = parse_config_file(v[1]);
        printf("dpi2en=%d dpi1=%u\n", (int)c.dpi[1].enabled, c.dpi[0].value);
    } catch (const std::exception& e) { printf("THREW %s\n", e.what()); }
    return 0;
}
EOF
g++ -std=c++17 $INC "$TMP/cfg.cpp" "$REPO/src/config.cpp" $SRC -o "$TMP/cfg" 2>/dev/null
chk "inline ';' comment stripped"        "dpi2en=0" "$("$TMP/cfg" "$TMP/a.ini")"
chk "color=#ff0000 survives (not a comment)" "dpi1=400" "$("$TMP/cfg" "$TMP/b.ini")"

hdr "Fire button encoding"
# Measured click counts on 25a7:fa07 for the value written to the mouse:
#   0 -> 1 click, 1 -> 1, 2 -> 2, 3 -> 3, 4 and above -> NOTHING fires.
# So "no clicks" can only be spelled 4, and 3 is a real ceiling rather than a
# cautious guess. Both facts are easy to "tidy away" later, hence these checks.
cat > "$TMP/fire.cpp" <<'EOF'
#include "data.h"
#include <cstdio>
int main() {
    const char* in[] = {"fire", "fire:58:0", "fire:58:1", "fire:58:2",
                        "fire:58:3", "fire:58:4", "fire:58:50"};
    for (auto s : in) {
        ActionBytes ab;
        if (!parse_action(s, ab)) { printf("%s=REJECT ", s); continue; }
        printf("%s=%02x%02x%02x%02x ", s, ab[0], ab[1], ab[2], ab[3]);
    }
    printf("\n");
    return 0;
}
EOF
g++ -std=c++17 $INC "$TMP/fire.cpp" $SRC -o "$TMP/fire" 2>/dev/null
FV="$("$TMP/fire")"
chk "bare 'fire' is speed 58 / 3 clicks"          "fire=043a0314"      "$FV"
chk "times=0 is written as 4 (only value that fires nothing)" "fire:58:0=043a0413" "$FV"
chk "times=1 passes through unchanged"            "fire:58:1=043a0116" "$FV"
chk "times=2 passes through unchanged"            "fire:58:2=043a0215" "$FV"
chk "times=3 passes through unchanged"            "fire:58:3=043a0314" "$FV"
chk "times=4 refused (hardware fires nothing)"    "fire:58:4=REJECT"   "$FV"
chk "times=50 refused (stored but never fires)"   "fire:58:50=REJECT"  "$FV"

hdr "Action name round-trip (every name --save could emit)"
# --save turns stored bytes back into names, and those names have to parse to
# the bytes they came from or a saved config silently differs from the mouse.
# The risk is aliases: several names share one encoding ("none"/"disable",
# "left" as a mouse button AND as an arrow keycode), so the reverse direction
# has to pick one spelling per encoding. This walks every name in the tables.
cat > "$TMP/names.cpp" <<'EOF'
#include "protocol.h"
#include "data.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

// Simulated device memory: apply a write packet the way the mouse would.
static uint8_t mem[0x400];
static void apply_packet(const Packet& p) {
    if (p[1] != 0x07) return;
    unsigned addr = (unsigned)((p[3] << 8) | p[4]), len = p[5];
    for (unsigned i = 0; i < len && addr + i < sizeof(mem); ++i)
        mem[addr + i] = p[6 + i];
}

int main() {
    int checked = 0, bad = 0;
    for (const std::string& name : all_parseable_action_names()) {
        ActionBytes want;
        if (!parse_action(name, want)) { printf("PARSEFAIL %s\n", name.c_str()); ++bad; continue; }

        std::string spelled;
        if (want[0] == 0x90 || want[0] == 0x92) {
            // Keyboard and consumer bindings live in the button's event list,
            // so go through the packets to get the bytes the mouse stores.
            memset(mem, 0xFF, sizeof mem);
            std::map<uint8_t, ActionBytes> m; m[0] = want;
            if (want[0] == 0x90 && want[3] > 1) register_multikey_action(0, name);
            for (const Packet& p : build_button_mapping(m, nullptr)) apply_packet(p);
            spelled = decode_key_event_list(&mem[0x0100], 20);
        } else {
            spelled = action_name(want);
        }

        ++checked;
        ActionBytes back;
        if (spelled.empty() || !parse_action(spelled, back) || back != want) {
            printf("MISMATCH '%s' -> '%s'\n", name.c_str(), spelled.c_str());
            ++bad;
        }
    }
    printf("NAMES checked=%d bad=%d\n", checked, bad);
    return 0;
}
EOF
g++ -std=c++17 $INC "$TMP/names.cpp" $SRC_ALL -o "$TMP/names" 2>/dev/null
NM="$("$TMP/names" 2>&1)"
chk "every action name survives bytes → name → bytes" "bad=0" "$NM"
# Guard against the harness silently walking an empty table.
NCHECKED="$(sed -n 's/.*checked=\([0-9]*\).*/\1/p' <<<"$NM")"
if [[ -n "$NCHECKED" && "$NCHECKED" -gt 100 ]]; then
  ok "the whole table was walked ($NCHECKED names)"
else
  no "suspiciously few names checked — $NM"
fi

hdr "Config round-trip (write path → device image → read path)"
# The strongest offline check there is: build the packets a Config would be
# written as, apply them to a simulated device memory, read that memory back
# through the same addresses M913_READ_CODES asks for, decode it, and require
# the result to match what went in. It covers both halves at once — a wrong
# address or a misread field on either side breaks it — and it is what makes
# --save trustworthy without a mouse to hand.
cat > "$TMP/rt.cpp" <<'EOF'
#include "config.h"
#include "readback.h"
#include "data.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

static uint8_t mem[0x400];
static void apply_packet(const Packet& p) {
    if (p[1] != 0x07) return;
    unsigned addr = (unsigned)((p[3] << 8) | p[4]), len = p[5];
    for (unsigned i = 0; i < len && addr + i < sizeof(mem); ++i)
        mem[addr + i] = p[6 + i];
}
// Read the image back exactly where the real read codes point, so a decoder
// that wants an address nobody asks for fails here rather than on hardware.
static BlockMap to_blocks() {
    BlockMap b;
    for (const Packet* req : config_read_requests()) {
        uint16_t addr = request_address(*req);
        std::array<uint8_t, READ_CHUNK> c{};
        for (size_t i = 0; i < READ_CHUNK; ++i)
            c[i] = (addr + i < sizeof(mem)) ? mem[addr + i] : 0xFF;
        b[addr] = c;
    }
    return b;
}

static int fails = 0;
static void eq(const char* what, long a, long b) {
    if (a != b) { printf("BAD %s: %ld != %ld\n", what, a, b); ++fails; }
}
// Buttons are compared as BYTES, not spellings: "three_click" and "fire:50:3"
// are the same four bytes, and the arrow keys come back as arrow_*.
static void eq_button(const char* tag, const Config& a, const Config& b,
                      const std::string& key) {
    ActionBytes wa{}, wb{};
    auto ia = a.buttons.find(key), ib = b.buttons.find(key);
    if (ia == a.buttons.end() || ib == b.buttons.end() ||
        !parse_action(ia->second, wa) || !parse_action(ib->second, wb) || wa != wb) {
        printf("BAD %s %s: '%s' -> '%s'\n", tag, key.c_str(),
               ia == a.buttons.end() ? "" : ia->second.c_str(),
               ib == b.buttons.end() ? "" : ib->second.c_str());
        ++fails;
    }
}

int main(int, char** argv) {
    const char* ini_path = argv[1];

    // One binding per branch of build_button_mapping(): direct mouse action,
    // fire with parameters, plain key, modifier+key, multi-key, modifier
    // only, two consumer keys, DPI controls, a special, and an arrow alias.
    const char* actions[16] = {
        "left", "right", "middle", "fire:25:2",
        "f5", "ctrl+c", "a+b+c", "super",
        "media_play", "media_vol_up", "dpi+", "dpi-cycle",
        "led_toggle", "none", "arrow_left", "www_back",
    };

    Config in;
    in.mouse.polling_rate = 500; in.mouse.set = true;
    const uint16_t vals[DPI_SLOTS] = {400, 800, 1600, 3200, 6400};
    for (int i = 0; i < DPI_SLOTS; ++i) in.dpi[i].value = vals[i];
    in.dpi[3].enabled = false;            // stages cascade off from here
    in.led.set = true; in.led.mode = LedMode::Respiration;
    in.led.color = 0x123456; in.led.brightness = 200; in.led.speed = 4;
    for (int i = 0; i < 16; ++i)
        in.buttons[button_ini_name(button_ini_order()[i])] = actions[i];

    memset(mem, 0xFF, sizeof mem);
    for (auto& seq : build_config_sequences(in, nullptr, false))
        for (const Packet& p : seq.packets) apply_packet(p);

    Config out; DecodeReport rep;
    if (!decode_device_config(to_blocks(), out, rep)) { printf("DECODEFAIL\n"); return 0; }

    eq("polling", in.mouse.polling_rate, out.mouse.polling_rate);
    for (int i = 0; i < DPI_SLOTS; ++i) eq("dpi", in.dpi[i].value, out.dpi[i].value);
    // The device stores a stage COUNT, not a per-slot mask, so the cascade is
    // the expected answer here, not the enabled[] that went in.
    int want_stages = compx_active_dpi_stage_count(
        {in.dpi[0].enabled, in.dpi[1].enabled, in.dpi[2].enabled,
         in.dpi[3].enabled, in.dpi[4].enabled});
    int got_stages = 0;
    for (int i = 0; i < DPI_SLOTS; ++i) if (out.dpi[i].enabled) ++got_stages;
    eq("stages", want_stages, got_stages);
    eq("led mode",  (long)in.led.mode, (long)out.led.mode);
    eq("led color", in.led.color, out.led.color);
    eq("led brightness", in.led.brightness, out.led.brightness);
    eq("led speed", in.led.speed, out.led.speed);
    for (int i = 0; i < 16; ++i)
        eq_button("button", in, out, button_ini_name(button_ini_order()[i]));

    // And what --save would write has to parse back to the same thing.
    { std::ofstream f(ini_path); f << config_to_ini(out, "round-trip test", rep.unnamed_buttons); }
    Config re = parse_config_file(ini_path);
    eq("ini polling", out.mouse.polling_rate, re.mouse.polling_rate);
    eq("ini led mode", (long)out.led.mode, (long)re.led.mode);
    eq("ini led color", out.led.color, re.led.color);
    eq("ini led speed", out.led.speed, re.led.speed);
    for (int i = 0; i < DPI_SLOTS; ++i) eq("ini dpi", out.dpi[i].value, re.dpi[i].value);
    int re_stages = 0;
    for (int i = 0; i < DPI_SLOTS; ++i) if (re.dpi[i].enabled) ++re_stages;
    eq("ini stages", want_stages, re_stages);
    for (int i = 0; i < 16; ++i)
        eq_button("ini button", out, re, button_ini_name(button_ini_order()[i]));

    printf("RT fails=%d warnings=%zu missing=%zu unnamed=%zu\n",
           fails, rep.warnings.size(), rep.missing.size(), rep.unnamed_buttons.size());

    // An erased device (every byte 0xFF) must decode to gaps, not to invented
    // values, and must still produce an INI that parses.
    memset(mem, 0xFF, sizeof mem);
    Config e; DecodeReport erep;
    decode_device_config(to_blocks(), e, erep);
    { std::ofstream f(std::string(ini_path) + ".erased");
      f << config_to_ini(e, "erased", erep.unnamed_buttons); }
    bool ini_ok = true;
    try { parse_config_file(std::string(ini_path) + ".erased"); }
    catch (const std::exception&) { ini_ok = false; }
    printf("ERASED buttons=%zu named=%zu iniparses=%d\n",
           erep.unnamed_buttons.size(), e.buttons.size(), (int)ini_ok);
    return 0;
}
EOF
g++ -std=c++17 $INC "$TMP/rt.cpp" $SRC_ALL -o "$TMP/rt" 2>/dev/null
RT="$("$TMP/rt" "$TMP/rt.ini" 2>&1)"
chk "config survives write → device → read → INI → parse" "RT fails=0" "$RT"
chk "nothing decoded with a warning"                      "warnings=0"  "$RT"
chk "every address the decoder wants is actually read"    "missing=0"   "$RT"
chk "all 16 buttons were named"                           "unnamed=0"   "$RT"
chk "an erased device names no buttons"                   "ERASED buttons=16 named=0" "$RT"
chk "an erased device still writes a parseable INI"       "iniparses=1" "$RT"

hdr "CLI surface"
chk "--profile is gone"          "unrecognized option" "$($CTL --profile 2 2>&1)"
chk "--probe-commands in --help" "--probe-commands"    "$($CTL --help 2>&1)"
chk "--get in --help"            "--get"               "$($CTL --help 2>&1)"
chk "--save in --help"           "--save"              "$($CTL --help 2>&1)"
chk "--led-color in --help"      "--led-color"         "$($CTL --help 2>&1)"
chk "whole-block writes documented in --help" "reset to its factory default" \
    "$($CTL --help 2>&1)"

# Every one of these is rejected during option parsing, before the device is
# opened — the same reason the --get bound is checked there. A value that only
# fails later could leave DPI or LED already written and the commit skipped.
chk "--led rejects an unknown mode"      "unknown LED mode"    "$($CTL --led purple 2>&1)"
chk "--led-color rejects non-hex"        "6 hex digits"        "$($CTL --led-color zzzzzz 2>&1)"
chk "--led-color rejects short input"    "6 hex digits"        "$($CTL --led-color f00 2>&1)"
chk "--led-brightness rejects 256"       "must be 0-255"       "$($CTL --led-brightness 256 2>&1)"
chk "--led-speed rejects 0"              "must be 1-5"         "$($CTL --led-speed 0 2>&1)"
for f in "--led purple" "--led-color zz" "--led-speed 9" "--led-brightness 999"; do
  chk "bad $f never opens the device" "0" "$($CTL $f 2>&1 | grep -c Connected)"
done

# --get indexes M913_READ_CODES directly, so an unbounded index would read past
# the table and transmit whatever followed it. The bound is checked during
# option parsing, which is also what keeps these checks device-free.
chk "--get rejects an out-of-range index"     "must be 0-68"  "$($CTL --get 69 2>&1)"
chk "--get rejects a non-numeric index"       "invalid --get" "$($CTL --get abc 2>&1)"
chk "--get rejects a trailing-garbage index"  "invalid --get" "$($CTL --get 5x 2>&1)"
chk "--get=N (attached) is bounded too"       "must be 0-68"  "$($CTL --get=999 2>&1)"
chk "a bad --get index never opens the device" "0" \
    "$($CTL --get 999 2>&1 | grep -c Connected)"

# ---------------------------------------------------------------- hardware
USBDEV=""
if [[ "$OFFLINE_ONLY" != "1" ]]; then USBDEV="$(find_usb_dev || true)"; fi

if [[ -z "$USBDEV" ]]; then
  hdr "Hardware"
  if [[ "$OFFLINE_ONLY" == "1" ]]; then sk "OFFLINE_ONLY=1 — device sections skipped"
  else sk "no supported mouse found — device sections skipped"; fi
else
  hdr "Endpoint selection (needs the device)"
  chk "--listen EP (separated)" "Endpoint: 0x82" "$(timeout 2 $CTL --listen 0x82 2>&1)"
  chk "--listen=EP (attached)"  "Endpoint: 0x82" "$(timeout 2 $CTL --listen=0x82 2>&1)"
  chk "bare --listen = both"    "Endpoints:"     "$(timeout 2 $CTL --listen 2>&1)"
  chk "unsupported --dpi refused with a suggestion" \
      "nearest supported value" "$($CTL --dpi 1=3100 2>&1)"
  chk "oversized combo sends nothing" "0" \
      "$($CTL --button side1=ctrl+shift+alt+f4 2>&1 | grep -c '^\s*-->')"
  chk "bad --button aborts before the device opens" "0" \
      "$($CTL --dpi 1=800 --button side1=ctrl+shift+alt+f4 2>&1 | grep -c Connected)"

  hdr "Signal handling and driver recovery ($USBDEV)"
  drv(){ local l; l="$(readlink "/sys/bus/usb/devices/${USBDEV}:1.$1/driver" 2>/dev/null)"
         if [[ -z "$l" ]]; then echo NONE; else basename "$l"; fi; }
  $CTL --listen >/dev/null 2>&1 & P=$!; sleep 2
  kill -TERM $P 2>/dev/null; wait $P 2>/dev/null; sleep 1
  chk "SIGTERM reattaches interface 0" "usbhid" "$(drv 0)"
  chk "SIGTERM reattaches interface 1" "usbhid" "$(drv 1)"
  $CTL --listen >/dev/null 2>&1 & P=$!; sleep 2
  kill -KILL $P 2>/dev/null; wait $P 2>/dev/null; sleep 1
  chk "SIGKILL strands it (cannot be caught)" "NONE" "$(drv 0)"
  $CTL --probe >/dev/null 2>&1; sleep 1
  chk "the next run reattaches interface 0" "usbhid" "$(drv 0)"
  chk "the next run reattaches interface 1" "usbhid" "$(drv 1)"

  hdr "Configuration read-back (--save, needs the device)"
  # The offline round-trip proves the decoder agrees with the packet builders.
  # What it cannot prove is that the ADDRESSES are right: those came from one
  # captured vendor session, so the only real check is to write a known config
  # and read it back off the mouse.
  VID="$(cat "/sys/bus/usb/devices/${USBDEV}/idVendor" 2>/dev/null || echo '')"
  if [[ "$VID" == "3554" ]]; then
    chk "--save refuses Compx rather than guessing" "Areson layout only" \
        "$($CTL --save 2>&1)"
  else
    printf '[mouse]\npolling_rate=500\n[dpi]\ndpi1=800\ndpi2=1600\ndpi3=3200\n' \
           > "$TMP/known.ini"
    printf '[led]\nmode=steady\ncolor=ff0000\nbrightness=200\n'              >> "$TMP/known.ini"
    printf '[buttons]\nbutton_side1=f5\nbutton_side2=ctrl+c\nbutton_side3=a+b+c\n' \
           >> "$TMP/known.ini"
    printf 'button_side4=media_play\nbutton_fire=fire:25:2\n'                >> "$TMP/known.ini"
    $CTL --config "$TMP/known.ini" >/dev/null 2>&1
    sleep 2

    # INI on stdout, progress on stderr — that split is what makes this work.
    SAVED="$($CTL --save 2>/dev/null)"
    if [[ -z "$SAVED" || "$SAVED" == *INCOMPLETE* ]]; then
      # Same reason the ACK count is reported rather than asserted: the replies
      # come from the mouse, and an idle 2.4G mouse answers late or not at all.
      printf "  \033[33mINFO\033[0m  %s\n" \
        "--save came back empty or incomplete — normal for an idle wireless mouse, move it and re-run"
    else
      chk "polling rate read back"      "polling_rate=500"     "$SAVED"
      chk "dpi1 read back"              "dpi1=800"             "$SAVED"
      chk "dpi3 read back"              "dpi3=3200"            "$SAVED"
      chk "LED mode read back"          "mode=steady"          "$SAVED"
      chk "LED colour read back"        "color=ff0000"         "$SAVED"
      chk "LED brightness read back"    "brightness=200"       "$SAVED"
      chk "plain key read back"         "button_side1=f5"      "$SAVED"
      chk "combo read back"             "button_side2=ctrl+c"  "$SAVED"
      chk "multi-key read back"         "button_side3=a+b+c"   "$SAVED"
      chk "multimedia read back"        "button_side4=media_play" "$SAVED"
      chk "fire parameters read back"   "button_fire=fire:25:2"   "$SAVED"
      chk "no undecodable bindings"     "0" "$(grep -c '^; button' <<<"$SAVED")"
      printf '%s' "$SAVED" > "$TMP/saved.ini"
      if $CTL --config "$TMP/saved.ini" >/dev/null 2>&1; then
        ok "--save output re-applies with --config"
      else
        no "--save output was rejected by --config"
      fi
    fi
  fi

  hdr "End-to-end"
  # The wireless link needs a moment after the claim/kill churn above,
  # otherwise ACKs time out and the counts below read as failures.
  sleep 3
  if [[ -f "$RESTORE_INI" ]]; then
    OUT="$($CTL --config "$RESTORE_INI" 2>&1)"; RC=$?
    chk "config applies" "0" "$RC"
    N="$(grep -c '^\s*-->' <<<"$OUT")"
    [[ "$N" -gt 0 ]] && ok "packets sent ($N)" || no "no packets sent"

    # ACKs are generated by the MOUSE, not the receiver, and a 2.4G mouse that
    # is sitting still answers slowly or not at all within the 1.5 s window.
    # That is expected and send_cmd() warns and continues, so this is reported
    # rather than asserted -- otherwise the result depends on whether someone
    # happened to be moving the mouse.
    A="$(grep -c 'no ACK' <<<"$OUT")"
    if [[ "$A" == "0" ]]; then
      ok "every packet ACKed ($N/$N)"
    else
      printf "  \033[33mINFO\033[0m  %s\n" \
        "$A/$N packets went unACKed — normal for an idle wireless mouse, move it to get ACKs"
    fi
    $CTL --config "$RESTORE_INI" >/dev/null 2>&1   # leave the mouse as found
  else
    sk "RESTORE_INI not found at $RESTORE_INI"
  fi
fi

printf "\n\033[1m%d passed, %d failed, %d skipped\033[0m\n" "$pass" "$fail" "$skip"
[[ "$fail" == "0" ]]
