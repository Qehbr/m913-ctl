#include "protocol.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include "data.h"

// Global storage for multi-key action strings (simple solution)
static std::map<uint8_t, std::string> g_multikey_actions;

void register_multikey_action(uint8_t button_idx, const std::string& action_str) {
    g_multikey_actions[button_idx] = action_str;
}

void clear_multikey_actions() {
    g_multikey_actions.clear();
}

// -----------------------------------------------------------------------
// Checksum
// -----------------------------------------------------------------------

uint8_t compute_checksum(const Packet& p) {
    // Host → device checksum formula, confirmed by cross-checking ALL
    // templates in mouse_m908's M913 data.cpp against their byte[16]:
    //   byte[16] = (0x55 - sum(bytes[0..15])) & 0xFF
    //
    // Note: device → host packets use a DIFFERENT formula:
    //   byte[16] = (0x4C - sum(bytes[1..15])) & 0xFF
    // (byte[0] = report ID 0x09 excluded from that sum)
    uint16_t s = 0;
    for (int i = 0; i < M913_PACKET_SIZE - 1; ++i)
        s += p[i];
    return static_cast<uint8_t>((0x55 - s) & 0xFF);
}

// -----------------------------------------------------------------------
// Internal data tables (from mouse_m908 M913 data.cpp / rd_mouse_wireless.cpp)
// -----------------------------------------------------------------------

// Default button-mapping packets (8 × 17 bytes).
// Two buttons per packet; addresses step by 0x08 from 0x60.
// Source: mouse_m913::_c_data_button_mapping
static const uint8_t default_button_mapping[8][17] = {
    {0x08,0x07,0x00,0x00,0x60,0x08, 0x00,0x00,0x00,0x55, 0x05,0x00,0x00,0x50, 0x00,0x00,0x34},
    {0x08,0x07,0x00,0x00,0x68,0x08, 0x05,0x00,0x00,0x50, 0x01,0x08,0x00,0x4c, 0x00,0x00,0x2c},
    {0x08,0x07,0x00,0x00,0x70,0x08, 0x05,0x00,0x00,0x50, 0x05,0x00,0x00,0x50, 0x00,0x00,0x24},
    {0x08,0x07,0x00,0x00,0x78,0x08, 0x01,0x02,0x00,0x52, 0x01,0x01,0x00,0x53, 0x00,0x00,0x1c},
    {0x08,0x07,0x00,0x00,0x80,0x08, 0x05,0x00,0x00,0x50, 0x05,0x00,0x00,0x50, 0x00,0x00,0x14},
    {0x08,0x07,0x00,0x00,0x88,0x08, 0x01,0x04,0x00,0x50, 0x04,0x3a,0x03,0x14, 0x00,0x00,0x0c},
    {0x08,0x07,0x00,0x00,0x90,0x08, 0x05,0x00,0x00,0x50, 0x05,0x00,0x00,0x50, 0x00,0x00,0x04},
    {0x08,0x07,0x00,0x00,0x98,0x08, 0x05,0x00,0x00,0x50, 0x05,0x00,0x00,0x50, 0x00,0x00,0xfc},
};

// Per-button address bytes for keyboard-key sub-packets (bytes [3] and [4]).
// Source: mouse_m913::_c_keyboard_key_buttons  (first two bytes of each entry)
static const uint8_t kb_key_addr[16][2] = {
    {0x01,0x00}, // button 0  (Side1)
    {0x01,0x20}, // button 1  (Side2)
    {0x01,0x40}, // button 2  (Side3)
    {0x01,0x60}, // button 3  (Side4)
    {0x01,0x80}, // button 4  (Side5)
    {0x01,0xa0}, // button 5  (Side6)
    {0x01,0xc0}, // button 6  (Right)
    {0x01,0xe0}, // button 7  (Left)
    {0x02,0x00}, // button 8  (Side7)
    {0x02,0x20}, // button 9  (Side8)
    {0x02,0x40}, // button 10 (Middle)
    {0x02,0x60}, // button 11 (Fire)
    {0x02,0x80}, // button 12 (Side9)
    {0x02,0xa0}, // button 13 (Side10)
    {0x02,0xc0}, // button 14 (Side11)
    {0x02,0xe0}, // button 15 (Side12)
};

// DPI code lookup table (DPI value → 3-byte encoding).
// Source: mouse_m913::_c_dpi_codes
// Only a representative subset; extend as needed.
static const struct { uint16_t dpi; uint8_t b[3]; } dpi_table[] = {
    {  100, {0x00,0x00,0x55}},
    {  200, {0x02,0x02,0x51}},
    {  300, {0x03,0x03,0x4f}},
    {  400, {0x04,0x04,0x4d}},
    {  500, {0x05,0x05,0x4b}},
    {  600, {0x06,0x06,0x49}},
    {  700, {0x07,0x07,0x47}},
    {  800, {0x09,0x09,0x43}},
    {  900, {0x0a,0x0a,0x41}},
    { 1000, {0x0b,0x0b,0x3f}},
    { 1100, {0x0c,0x0c,0x3d}},
    { 1200, {0x0d,0x0d,0x3b}},
    { 1300, {0x0e,0x0e,0x39}},
    { 1400, {0x10,0x10,0x35}},
    { 1500, {0x11,0x11,0x33}},
    { 1600, {0x12,0x12,0x31}},
    { 1700, {0x13,0x13,0x2f}},
    { 1800, {0x14,0x14,0x2d}},
    { 1900, {0x16,0x16,0x29}},
    { 2000, {0x17,0x17,0x27}},
    { 2100, {0x18,0x18,0x25}},
    { 2200, {0x19,0x19,0x23}},
    { 2300, {0x1a,0x1a,0x21}},
    { 2400, {0x1b,0x1b,0x1f}},
    { 2500, {0x1d,0x1d,0x1b}},
    { 2600, {0x1e,0x1e,0x19}},
    { 2700, {0x1f,0x1f,0x17}},
    { 2800, {0x20,0x20,0x15}},
    { 2900, {0x21,0x21,0x13}},
    { 3000, {0x23,0x23,0x0f}},
    { 3200, {0x26,0x26,0x09}},
    { 3600, {0x2a,0x2a,0x01}},
    { 4000, {0x2f,0x2f,0xf7}},
    { 4800, {0x39,0x39,0xe3}},
    { 5000, {0x3b,0x3b,0xdf}},
    { 5500, {0x41,0x41,0xd3}},
    { 6000, {0x47,0x47,0xc7}},
    { 6400, {0x4c,0x4c,0xbd}},
    { 6600, {0x4f,0x4f,0xb7}},
    { 7000, {0x53,0x53,0xaf}},
    { 7200, {0x56,0x56,0xa9}},
    { 7300, {0x57,0x57,0xa7}},
    { 7400, {0x58,0x58,0xa5}},
    { 7500, {0x59,0x59,0xa3}},
    { 8000, {0x5f,0x5f,0x97}},
    { 8500, {0x65,0x65,0x8b}},
    { 9000, {0x6b,0x6b,0x7f}},
    { 9600, {0x73,0x73,0x6f}},
    {10000, {0x77,0x77,0x67}},
    {11000, {0x83,0x83,0x4f}},
    {12000, {0x8f,0x8f,0x37}},
    {13000, {0x9b,0x9b,0x1f}},
    {14000, {0xa7,0xa7,0x07}},
    {15000, {0xb3,0xb3,0xef}},
    {16000, {0xbd,0xbd,0xdb}},
};

// Keyboard-key sub-packet template.
// Source: rd_mouse_wireless::_c_data_button_as_keyboard_key
static const uint8_t kb_key_template[17] = {
    0x08,0x07,0x00,0x01,0x60,0x08,
    0x02,0x81,0x21,0x00,0x41,0x21,0x00,0x4f,
    0x00,0x00,0x88
};

// DPI config packet templates (4 packets).
// Source: mouse_m913::_c_data_dpi
static const uint8_t dpi_template[4][17] = {
    {0x08,0x07,0x00,0x00,0x0c,0x08, 0x00,0x00,0x00,0x55, 0x02,0x02,0x00,0x51, 0x00,0x00,0x88},
    {0x08,0x07,0x00,0x00,0x14,0x08, 0x03,0x03,0x00,0x4f, 0x04,0x04,0x00,0x4d, 0x00,0x00,0x80},
    {0x08,0x07,0x00,0x00,0x1c,0x04, 0x05,0x05,0x00,0x4b, 0x00,0x00,0x00,0x00, 0x00,0x00,0xd1},
    {0x08,0x07,0x00,0x00,0x02,0x02, 0x05,0x50,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0xed},
};

// "Unknown_2" packets always sent after DPI config.
// Source: mouse_m913::_c_data_unknown_2
static const uint8_t unknown2[3][17] = {
    {0x08,0x07,0x00,0x00,0x2c,0x08, 0xff,0x00,0x00,0x56, 0x00,0x00,0xff,0x56, 0x00,0x00,0x68},
    {0x08,0x07,0x00,0x00,0x34,0x08, 0x00,0xff,0x00,0x56, 0xff,0xff,0x00,0x57, 0x00,0x00,0x60},
    {0x08,0x07,0x00,0x00,0x3c,0x04, 0xff,0x55,0x7d,0x84, 0x00,0x00,0x00,0x00, 0x00,0x00,0xb1},
};

// LED mode packet templates.
// The 0x0000 (polling rate) packet has been removed from all templates —
// it is now sent separately via build_polling_rate_packet().
// Source: mouse_m913::_c_data_led_*
static const uint8_t led_off[1][17] = {
    {0x08,0x07,0x00,0x00,0x58,0x02, 0x00,0x55,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x97},
};
static const uint8_t led_breathing[2][17] = {
    {0x08,0x07,0x00,0x00,0x54,0x08, 0xff,0x00,0x00,0x57, 0x01,0x54,0xff,0x56, 0x00,0x00,0xeb},
    {0x08,0x07,0x00,0x00,0x5c,0x02, 0x03,0x52,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x93},
};
static const uint8_t led_rainbow[2][17] = {
    {0x08,0x07,0x00,0x00,0x54,0x08, 0xff,0x00,0xff,0x57, 0x03,0x52,0x80,0xd5, 0x00,0x00,0xeb},
    {0x08,0x07,0x00,0x00,0x5c,0x02, 0x03,0x52,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x93},
};
static const uint8_t led_static[1][17] = {
    {0x08,0x07,0x00,0x00,0x54,0x08, 0xff,0x00,0x00,0x57, 0x01,0x54,0xff,0x56, 0x00,0x00,0xeb},
};

// -----------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------

static Packet from_raw(const uint8_t src[17]) {
    Packet p;
    std::copy(src, src + 17, p.begin());
    return p;
}

// Look up DPI 3-byte encoding; returns nullptr if not found.
static const uint8_t* lookup_dpi(uint16_t dpi) {
    for (auto& e : dpi_table)
        if (e.dpi == dpi) return e.b;
    return nullptr;
}

// -----------------------------------------------------------------------
// Button mapping
// -----------------------------------------------------------------------

std::vector<Packet> build_button_mapping(
        const std::map<uint8_t, ActionBytes>& changes) {

    // Start from the default 8-packet template.
    Packet buf[8];
    for (int i = 0; i < 8; ++i)
        buf[i] = from_raw(default_button_mapping[i]);

    // The action bytes of button `b` sit at:
    //   packet[b/2], bytes [6..9]  if b is even
    //   packet[b/2], bytes [10..13] if b is odd
    static const uint8_t kb_key_action[4] = {0x05, 0x00, 0x00, 0x50};

    // Keyboard-key sub-packets are collected first.
    std::vector<Packet> result;

    for (auto& [btn_idx, ab] : changes) {
        if (btn_idx >= 16) continue;  // out of range

        if (ab[0] == 0x90 || ab[0] == 0x92) {
            // Keyboard-key action: ab = {0x90, modifier, scancode, 0x00} (single key)
            //                  or  ab = {0x91, modifier, first_key, key_count} (multi-key)  
            //                  or  ab = {0x92, extra_byte, consumer_code, extra_byte2} (multimedia)
            // Confirmed protocol (from USB captures):
            //
            //   Plain key (no modifier): single 17-byte sub-packet at btn_addr, len=0x08
            //     payload: [0x02][0x81][SC][0x00][0x41][SC][0x00][inner_cksum]
            //     where 0x02=count(2 events), 0x81=key-down, 0x41=key-up
            //     inner_cksum = (0x55 - 0x02 - 0x81 - SC - 0x41 - SC) & 0xFF
            //                 = (0x91 - 2*SC) & 0xFF
            //
            //   Modifier+key: two sub-packets
            //     Event types: 0x80/0x40 = modifier down/up, 0x81/0x41 = key down/up
            //     Event order: [each mod bit down], key-down, [each mod bit up], key-up
            //     Packet 1 at btn_addr,        len=0x0A: [COUNT][first 3 events (9 bytes)]
            //     Packet 2 at btn_addr+0x0A,   len=var:  [remaining events][inner_cksum]
            //     inner_cksum = (0x55 - COUNT - sum_of_all_event_bytes) & 0xFF
            //
            // The mapping slot always gets the same KB marker (05 00 00 50).

            uint8_t addr_hi = kb_key_addr[btn_idx][0];
            uint8_t addr_lo = kb_key_addr[btn_idx][1];

            if (ab[0] == 0x92) {
                // Multimedia key: generate sub-packet with consumer code
                // Pattern: 08 07 00 01 ADDR 08 02 82 CODE EXTRA 42 CODE EXTRA CKSUM 00 00 XX
                uint8_t extra = ab[1];
                uint8_t code = ab[2];
                uint8_t extra2 = ab[3];
                
                Packet sub = {};
                sub[0]=0x08; sub[1]=0x07; sub[2]=0x00; sub[3]=addr_hi; sub[4]=addr_lo; sub[5]=0x08;
                sub[6]=0x02; sub[7]=0x82; sub[8]=code; sub[9]=extra; sub[10]=0x42; sub[11]=code; sub[12]=extra2;
                // Inner checksum: (0x55 - sum(bytes 6-12)) & 0xFF
                uint8_t isum = 0x02 + 0x82 + code + extra + 0x42 + code + extra2;
                sub[13] = (0x55u - isum) & 0xFF;
                sub[16] = compute_checksum(sub);
                result.push_back(sub);
            } else {
                uint8_t mods = ab[1];
                uint8_t sc   = ab[2];
                uint8_t key_count = ab[3];

                // Collect all keys for this binding.
                // For multi-key combos (key_count > 1), re-parse the
                // original action string to get all scancodes + modifiers.
                std::vector<uint8_t> keys;

                if (key_count > 1) {
                    auto it = g_multikey_actions.find(btn_idx);
                    if (it != g_multikey_actions.end()) {
                        uint8_t parsed_mods;
                        std::vector<uint8_t> parsed_keys;
                        if (parse_multikey(it->second, parsed_mods, parsed_keys) && parsed_keys.size() >= 2) {
                            mods = parsed_mods;
                            keys = parsed_keys;
                        }
                    }
                    // Fall back to single key if multi-key parsing failed
                    if (keys.empty()) {
                        keys.push_back(sc);
                    }
                } else {
                    if (sc != 0x00) keys.push_back(sc);
                }

                if (mods == 0x00 && keys.size() == 1) {
                    // Plain single key: use the existing single-packet template.
                    Packet sub;
                    std::copy(kb_key_template, kb_key_template + 17, sub.begin());
                    sub[3]  = addr_hi;
                    sub[4]  = addr_lo;
                    sub[8]  = keys[0];
                    sub[11] = keys[0];
                    sub[13] = static_cast<uint8_t>((0x91 - 2u * keys[0]) & 0xFF);
                    sub[16] = compute_checksum(sub);
                    result.push_back(sub);
                } else {
                    // Unified path for: modifier+key, multi-key, modifier+multi-key.
                    // Build event list following the pattern confirmed by USB captures:
                    //   1. All modifier bits DOWN (0x80, LSB first)
                    //   2. All regular keys DOWN (0x81, in order)
                    //   3. All modifier bits UP (0x40, same order as down)
                    //   4. All regular keys UP (0x41, REVERSE order)
                    static const uint8_t mod_bits[] = {
                        0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80
                    };

                    std::vector<uint8_t> evts;
                    evts.reserve(24);

                    // Modifier-down events
                    for (uint8_t b : mod_bits)
                        if (mods & b) { evts.push_back(0x80); evts.push_back(b); evts.push_back(0x00); }
                    // All keys down
                    for (uint8_t k : keys) {
                        evts.push_back(0x81); evts.push_back(k); evts.push_back(0x00);
                    }
                    // Modifier-up events (same order as down)
                    for (uint8_t b : mod_bits)
                        if (mods & b) { evts.push_back(0x40); evts.push_back(b); evts.push_back(0x00); }
                    // All keys up (reverse order)
                    for (int i = static_cast<int>(keys.size()) - 1; i >= 0; --i) {
                        evts.push_back(0x41); evts.push_back(keys[i]); evts.push_back(0x00);
                    }

                    uint8_t count = static_cast<uint8_t>(evts.size() / 3);

                    // Inner checksum covers COUNT + all event bytes
                    uint16_t isum = count;
                    for (uint8_t b : evts) isum += b;
                    uint8_t inner = (0x55u - static_cast<uint8_t>(isum & 0xFF)) & 0xFF;

                    // Packet 1: len=0x0A, payload = COUNT + first 9 event bytes
                    Packet p1 = {};
                    p1[0]=0x08; p1[1]=0x07; p1[2]=0x00;
                    p1[3]=addr_hi; p1[4]=addr_lo; p1[5]=0x0a;
                    p1[6] = count;
                    for (int i = 0; i < 9 && i < static_cast<int>(evts.size()); ++i)
                        p1[7 + i] = evts[i];
                    p1[16] = compute_checksum(p1);
                    result.push_back(p1);

                    // Packet 2: remaining event bytes + inner checksum
                    size_t p1_evts = std::min<size_t>(9, evts.size());
                    size_t remaining = evts.size() - p1_evts;
                    Packet p2 = {};
                    p2[0]=0x08; p2[1]=0x07; p2[2]=0x00;
                    p2[3]=addr_hi; p2[4]=static_cast<uint8_t>(addr_lo + 0x0a);
                    p2[5] = static_cast<uint8_t>(remaining + 1);
                    for (size_t i = 0; i < remaining; ++i)
                        p2[6 + i] = evts[p1_evts + i];
                    p2[6 + remaining] = inner;
                    p2[16] = compute_checksum(p2);
                    result.push_back(p2);
                }
            }

            // Put the keyboard_key marker in the mapping packet.
            int pkt = btn_idx / 2;
            int off = (btn_idx % 2 == 0) ? 6 : 10;
            for (int k = 0; k < 4; ++k)
                buf[pkt][off + k] = kb_key_action[k];
        } else {
            // Direct action (mouse button, DPI cycle, etc.)
            int pkt = btn_idx / 2;
            int off = (btn_idx % 2 == 0) ? 6 : 10;
            for (int k = 0; k < 4; ++k)
                buf[pkt][off + k] = ab[k];
        }
    }

    // Recompute checksum for any modified mapping packets.
    for (int i = 0; i < 8; ++i)
        buf[i][16] = compute_checksum(buf[i]);

    // Keyboard-key sub-packets go first, then the 8 mapping packets.
    for (int i = 0; i < 8; ++i)
        result.push_back(buf[i]);

    // Clean up multi-key registrations after use.
    clear_multikey_actions();

    return result;
}

// -----------------------------------------------------------------------
// DPI
// -----------------------------------------------------------------------

std::vector<Packet> build_dpi_packets(const DpiSettings& dpi) {
    // Copy the 4-packet template.
    Packet buf[4];
    for (int i = 0; i < 4; ++i)
        buf[i] = from_raw(dpi_template[i]);

    // Helper: set a DPI level's 3 bytes at the appropriate positions.
    auto set_level = [&](int pkt, int base_off, uint16_t val) {
        const uint8_t* code = lookup_dpi(val);
        if (!code) return;  // unknown DPI, keep template value
        buf[pkt][base_off]     = code[0];
        buf[pkt][base_off + 1] = code[1];
        // byte 2 is at +3 (not +2 — there's a 0x00 gap at +2)
        buf[pkt][base_off + 3] = code[2];
    };

    // Levels 1 and 2 → packet 0
    if (dpi.values[0]) set_level(0, 6,  dpi.values[0]);
    if (dpi.values[1]) set_level(0, 10, dpi.values[1]);
    // Levels 3 and 4 → packet 1
    if (dpi.values[2]) set_level(1, 6,  dpi.values[2]);
    if (dpi.values[3]) set_level(1, 10, dpi.values[3]);
    // Level 5 → packet 2 (only 4 bytes, no second level)
    if (dpi.values[4]) set_level(2, 6,  dpi.values[4]);

    // Enabled levels → packet 3 bytes [6] and [7].
    // mouse_m908 logic: highest disabled level determines the enable bytes.
    // Count how many levels are enabled (must keep at least 1).
    int num_enabled = 0;
    for (int i = 0; i < 5; ++i) if (dpi.enabled[i]) ++num_enabled;
    if (num_enabled == 0) {
        // Can't disable all levels; leave packet 3 at template default.
    } else {
        uint8_t e1 = 0x05, e2 = 0x50;
        if (!dpi.enabled[4]) { e1 = 0x04; e2 = 0x51; }
        if (!dpi.enabled[3]) { e1 = 0x03; e2 = 0x52; }
        if (!dpi.enabled[2]) { e1 = 0x02; e2 = 0x53; }
        if (!dpi.enabled[1]) { e1 = 0x01; e2 = 0x54; }
        buf[3][6] = e1;
        buf[3][7] = e2;
    }

    // Recompute checksums.
    for (int i = 0; i < 4; ++i)
        buf[i][16] = compute_checksum(buf[i]);

    std::vector<Packet> result;
    for (int i = 0; i < 4; ++i)
        result.push_back(buf[i]);

    // Append the 3 "unknown_2" packets (always required after DPI config).
    for (int i = 0; i < 3; ++i)
        result.push_back(from_raw(unknown2[i]));

    return result;
}

// -----------------------------------------------------------------------
// LED
// -----------------------------------------------------------------------

std::vector<Packet> build_led_packets(LedMode mode,
                                      uint32_t color,
                                      uint8_t  brightness,
                                      uint8_t  speed) {
    std::vector<Packet> result;

    if (mode == LedMode::Off) {
        result.push_back(from_raw(led_off[0]));

    } else if (mode == LedMode::Respiration) {
        // Respiration mode: color + mode in 0x54 packet, speed in 0x5C packet.
        // Each data byte has an inline checksum: (0x55 - data) & 0xFF
        // Confirmed from USB captures (logo_respiration_*.txt).
        uint8_t r = (color >> 16) & 0xff;
        uint8_t g = (color >> 8) & 0xff;
        uint8_t b = color & 0xff;

        Packet p1 = from_raw(led_breathing[0]);
        p1[6]  = r;
        p1[7]  = g;
        p1[8]  = b;
        p1[9]  = static_cast<uint8_t>((0x55 - r - g - b) & 0xFF);
        p1[10] = 0x02;  // Mode: respiration
        p1[11] = static_cast<uint8_t>((0x55 - 0x02) & 0xFF);  // = 0x53
        p1[12] = brightness;
        p1[13] = static_cast<uint8_t>((0x55 - brightness) & 0xFF);
        p1[16] = compute_checksum(p1);
        result.push_back(p1);

        // Speed packet at address 0x5C: [speed, (0x55 - speed)]
        // USB captures show speed range: 01 (slowest) to 05 (fastest).
        Packet p2 = from_raw(led_breathing[1]);
        p2[6] = speed;
        p2[7] = static_cast<uint8_t>((0x55 - speed) & 0xFF);
        p2[16] = compute_checksum(p2);
        result.push_back(p2);

    } else if (mode == LedMode::Rainbow) {
        for (int i = 0; i < 2; ++i)
            result.push_back(from_raw(led_rainbow[i]));

    } else {  // Steady (static color with brightness)
        // Inline checksums confirmed from USB captures (logo_steady_*.txt).
        uint8_t r = (color >> 16) & 0xff;
        uint8_t g = (color >>  8) & 0xff;
        uint8_t b =  color        & 0xff;

        Packet buf = from_raw(led_static[0]);
        buf[6]  = r;
        buf[7]  = g;
        buf[8]  = b;
        buf[9]  = static_cast<uint8_t>((0x55 - r - g - b) & 0xFF);
        buf[10] = 0x01; // Mode: steady
        buf[11] = static_cast<uint8_t>((0x55 - 0x01) & 0xFF);  // = 0x54
        buf[12] = brightness;
        buf[13] = static_cast<uint8_t>((0x55 - brightness) & 0xFF);
        buf[16] = compute_checksum(buf);
        result.push_back(buf);
        return result;
    }

    return result;
}

// -----------------------------------------------------------------------
// Polling rate
// -----------------------------------------------------------------------

Packet build_polling_rate_packet(uint16_t hz) {
    // Polling rate register is at address 0x0000, length 2.
    // Encoding confirmed from USB captures (1000.txt / 500.txt / 125.txt):
    //   1000 Hz → 0x01  250 Hz → 0x04
    //    500 Hz → 0x02  125 Hz → 0x08
    // Byte[7] is always (0x55 - code), making the outer checksum 0xEF for any rate.
    uint8_t code;
    if      (hz >= 1000) code = 0x01;
    else if (hz >= 500)  code = 0x02;
    else if (hz >= 250)  code = 0x04;
    else                 code = 0x08;  // 125 Hz

    Packet p{};
    p[0] = 0x08; p[1] = 0x07;
    p[4] = 0x00; p[5] = 0x02;
    p[6] = code;
    p[7] = static_cast<uint8_t>((0x55u - code) & 0xFF);
    p[16] = compute_checksum(p);
    return p;
}

// -----------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------

void hexdump_packet(const Packet& p, const std::string& label) {
    if (!label.empty())
        std::cout << label << "\n";

    std::cout << std::hex << std::setfill('0');
    for (int i = 0; i < M913_PACKET_SIZE; ++i) {
        std::cout << std::setw(2) << static_cast<int>(p[i]);
        if (i < M913_PACKET_SIZE - 1) std::cout << " ";
    }
    std::cout << std::dec << "\n";
}
