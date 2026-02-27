#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "usb.h"

// -----------------------------------------------------------------------
// M913 17-byte packet layout (confirmed from mouse_m908 M913 source +
// live USB captures)
//
//  Byte  | Role
//  ------|------------------------------------------------------------------
//   0    | Always 0x08 (config command marker)
//   1    | Sub-command (0x07 = write data, 0x04 = finalize)
//   2    | Always 0x00
//   3    | Always 0x00 (except keyboard-key sub-packets where it's an address byte)
//   4    | Memory address byte (which block to write)
//   5    | Payload length (bytes 6..5+len are meaningful)
//   6-13 | Payload data
//  14-15 | Padding (0x00)
//  16    | Checksum: (0x55 - sum(bytes[0..15])) & 0xFF
//
// Checksum formula confirmed by cross-checking ALL templates from
// mouse_m908 M913 data.cpp against their precomputed byte[16] values.
// -----------------------------------------------------------------------

// LED modes supported by the M913
enum class LedMode : uint8_t {
    Off        = 0x00,
    Steady     = 0x01,  // Static color with brightness control
    Respiration= 0x02,  // Breathing effect with speed control  
    Rainbow    = 0x03,  // Rainbow cycle effect
};

// M913 has two hardware profiles (switched by hardware button on bottom)
enum class Profile : uint8_t {
    P1 = 0x00,
    P2 = 0x01,
};

// Button indices matching mouse_m908's _c_button_names for the M913.
// These are the indices used in the 8-packet button-mapping sequence.
//
//  Mouse_m908 name  → physical button
//  button_1..6      → 12 side buttons (rows top-to-bottom on right side)
//  button_right     → right click
//  button_left      → left click
//  button_7..12     → more side buttons
//  button_middle    → middle click / scroll wheel click
//  button_fire      → fire button (near left click)
enum class Button : uint8_t {
    Side1  =  0,   // button_1  (side row)
    Side2  =  1,   // button_2
    Side3  =  2,   // button_3
    Side4  =  3,   // button_4
    Side5  =  4,   // button_5
    Side6  =  5,   // button_6
    Right  =  6,   // button_right
    Left   =  7,   // button_left
    Side7  =  8,   // button_7
    Side8  =  9,   // button_8
    Middle = 10,   // button_middle
    Fire   = 11,   // button_fire
    Side9  = 12,   // button_9
    Side10 = 13,   // button_10
    Side11 = 14,   // button_11
    Side12 = 15,   // button_12
};

// DPI level slot (1–5), 1-indexed for user-facing API
using DpiSlot = uint8_t;

// A raw 17-byte packet
using Packet = std::array<uint8_t, M913_PACKET_SIZE>;

// 4-byte action code for button remapping (see data.h for format)
using ActionBytes = std::array<uint8_t, 4>;

// -----------------------------------------------------------------------
// Packet sequence builders
//
// Each builder returns a std::vector<Packet> containing all the packets
// that must be sent (with ACK read after each) to apply the setting.
// -----------------------------------------------------------------------

// Helper to register multi-key action strings for complex parsing
void register_multikey_action(uint8_t button_idx, const std::string& action_str);
void clear_multikey_actions();

// Build the complete button-mapping packet sequence (always 8 packets
// plus any keyboard-key sub-packets that precede them).
//
// changes: map of button index → 4-byte action.
//   - Mouse/special actions (ab[0] != 0x90): used directly in the mapping.
//   - Keyboard-key actions (ab[0] == 0x90): a keyboard-key sub-packet is
//     prepended and {0x05,0x00,0x00,0x50} is used in the mapping packet.
//
// Buttons NOT in `changes` keep their factory-default actions.
std::vector<Packet> build_button_mapping(
    const std::map<uint8_t, ActionBytes>& changes);

// DPI settings for build_dpi_packets.
// value == 0 → keep the template default for that slot.
struct DpiSettings {
    std::array<uint16_t, 5> values  = {0, 0, 0, 0, 0};
    std::array<bool,     5> enabled = {true, true, true, true, true};
};

// Build the complete DPI packet sequence (4 DPI config packets +
// 3 "unknown_2" packets that must always follow, = 7 total).
std::vector<Packet> build_dpi_packets(const DpiSettings& dpi);

// Build the LED configuration packet sequence (1–2 packets).
// color: 24-bit RGB (0xRRGGBB), brightness: 0–255 (Steady mode only)
// speed: 1–5 (Respiration mode, 1=slowest, 5=fastest)
std::vector<Packet> build_led_packets(LedMode mode,
                                      uint32_t color      = 0x00ff00,
                                      uint8_t  brightness = 0xff,
                                      uint8_t  speed      = 0x03);

// Build the polling rate configuration packet (1 packet).
// hz: one of 125, 250, 500, 1000 (values are rounded down to nearest valid rate)
Packet build_polling_rate_packet(uint16_t hz);

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Compute the checksum byte for a host→device packet.
// Formula: (0x55 - sum(bytes[0..15])) & 0xFF
// Confirmed against all mouse_m908 M913 template values.
uint8_t compute_checksum(const Packet& p);

// Pretty-print a packet as a hex dump to stdout
void hexdump_packet(const Packet& p, const std::string& label = "");
