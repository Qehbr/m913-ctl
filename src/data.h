#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// 4-byte action encoding used in button remap packets.
// Format matches mouse_m908's keycode table:
//   [0]: function type  (0x81-0x8c = mouse action, 0x90 = keyboard key)
//   [1]: modifier byte  (bit flags: ctrl=0x01, shift=0x02, alt=0x04, super=0x08)
//   [2]: key scan code  (USB HID keyboard usage value)
//   [3]: reserved / extra
using ActionBytes = std::array<uint8_t, 4>;

// Returns the 4-byte action code for a given action name string.
// Supports:
//   - Mouse buttons: "left", "right", "middle", "forward", "backward"
//   - DPI controls:  "dpi+", "dpi-", "dpi-cycle"
//   - Special:       "led_toggle", "none", "three_click", "polling_switch"
//   - Fire button:   "fire:speed:times" where speed=3-255, times=0-3
//   - Multimedia:    "media_play", "media_next", "media_vol_up", etc.
//   - Keyboard keys: "a"-"z", "f1"-"f24", "0"-"9", "ctrl_l", "shift_l", etc.
//   - Combos:        "ctrl_l+c", "ctrl_l+shift_l+z", "a+b+c", etc.
//
// Returns false if the action string is not recognized.
bool parse_action(const std::string& action, ActionBytes& out);

// Helper function to parse multi-key combinations for protocol.cpp
// Returns list of key codes for a multi-key combination like "a+b+c"
bool parse_multikey(const std::string& action, uint8_t& mods, std::vector<uint8_t>& keys);

// The action bytes a button stores when its real binding lives in that
// button's keyboard event list at 0x0100+ (see kb_key_addr in protocol.cpp).
// action_name() deliberately does NOT resolve this one: the 4 bytes say
// "look elsewhere", so the caller has to read that list and call
// decode_key_event_list() instead.
static constexpr ActionBytes KB_LIST_MARKER = {0x05, 0x00, 0x00, 0x50};

// Canonical action name for a 4-byte action code — the inverse of
// parse_action(), used to turn a config read back off the mouse into an INI.
//
// Returns "" when the bytes are not recognised, including for
// KB_LIST_MARKER. Aliases are never returned: where several names share one
// encoding ("none"/"disable", "dpi-cycle"/"dpi-loop") the canonical spelling
// wins, so the result always parses back to the same bytes. The round-trip
// check in tests/regress.sh is what keeps that true as the tables grow.
std::string action_name(const ActionBytes& action);

// Decode a stored keyboard/consumer event list — as read back from the
// mouse — into an action string parse_action() accepts ("ctrl+c", "a+b+c",
// "media_play").
//
// `p` points at the list's count byte, `n` is how many bytes are readable
// from there. Returns "" if the list is malformed, which is also what an
// erased (all-0xFF) slot gives: 0xFF events would need 767 bytes.
std::string decode_key_event_list(const uint8_t* p, size_t n);

// Every name parse_action() accepts, canonical spellings and aliases alike.
// Exposed for the round-trip test in tests/regress.sh — nothing in the tool
// itself enumerates actions this way.
std::vector<std::string> all_parseable_action_names();

// Number of modifiers + keys a parsed action combines ("ctrl+shift+z" → 3).
// Returns 0 for actions that are not keyboard bindings (mouse buttons, DPI
// controls, multimedia, fire), which have no such limit.
//
// Compare against MAX_COMBO_TOKENS (protocol.h) to reject a binding before
// any packet is built — the packet builder cannot fit more than that.
size_t action_combo_tokens(const ActionBytes& action);

// Print all recognized action names to stdout (for --list-actions)
void list_actions();
