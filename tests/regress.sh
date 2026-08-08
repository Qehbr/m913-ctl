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

hdr "CLI surface"
chk "--profile is gone"          "unrecognized option" "$($CTL --profile 2 2>&1)"
chk "--probe-commands in --help" "--probe-commands"    "$($CTL --help 2>&1)"

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
