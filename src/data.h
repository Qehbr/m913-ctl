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

// Print all recognized action names to stdout (for --list-actions)
void list_actions();