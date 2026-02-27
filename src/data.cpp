#include "data.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <vector>

// -----------------------------------------------------------------------
// Mouse / special function actions
// Confirmed from rd_mouse_wireless::_c_keycodes (mouse_m908 source).
// -----------------------------------------------------------------------
static const std::map<std::string, ActionBytes> mouse_actions = {
    {"left",           {0x01, 0x01, 0x00, 0x53}},
    {"right",          {0x01, 0x02, 0x00, 0x52}},
    {"middle",         {0x01, 0x04, 0x00, 0x50}},
    {"backward",       {0x01, 0x08, 0x00, 0x4c}},
    {"forward",        {0x01, 0x10, 0x00, 0x44}},
    {"dpi-",           {0x02, 0x03, 0x00, 0x50}},
    {"dpi+",           {0x02, 0x02, 0x00, 0x51}},
    {"dpi-cycle",      {0x02, 0x01, 0x00, 0x52}},
    {"dpi-loop",       {0x02, 0x01, 0x00, 0x52}},  // alias
    {"led_toggle",     {0x08, 0x00, 0x00, 0x4d}},
    {"rgb_toggle",     {0x08, 0x00, 0x00, 0x4d}},  // alias
    {"none",           {0x00, 0x00, 0x00, 0x55}},
    {"disable",        {0x00, 0x00, 0x00, 0x55}},  // alias
    // "fire" = rapid-fire (hardware auto-repeat).
    // Confirmed from USB capture: bytes are 04 3a 03 14 (not 04 14 03 3a as in mouse_m908 source).
    {"fire",           {0x04, 0x3a, 0x03, 0x14}},
    // New actions from M913 captures
    {"three_click",    {0x04, 0x32, 0x03, 0x1c}},
    {"polling_switch", {0x07, 0x00, 0x00, 0x4e}},
    // Multimedia actions - these use keyboard sub-packet mechanism 
    // 0x92 marker indicates multimedia key that needs special sub-packet handling
    {"media_play",     {0x92, 0x00, 0xcd, 0x00}},
    {"media_player",   {0x92, 0x01, 0x83, 0x01}},  // Launch media player app
    {"media_next",     {0x92, 0x00, 0xb5, 0x00}}, 
    {"media_prev",     {0x92, 0x00, 0xb6, 0x00}},
    {"media_stop",     {0x92, 0x00, 0xb7, 0x00}},
    {"media_vol_up",   {0x92, 0x00, 0xe9, 0x00}},
    {"media_vol_down", {0x92, 0x00, 0xea, 0x00}},
    {"media_mute",     {0x92, 0x00, 0xe2, 0x00}},
    // Application launch actions
    {"media_email",    {0x92, 0x01, 0x8a, 0x01}},
    {"media_calc",     {0x92, 0x01, 0x92, 0x01}},
    {"media_computer", {0x92, 0x01, 0x94, 0x01}},
    {"media_home",     {0x92, 0x02, 0x23, 0x02}},
    {"media_search",   {0x92, 0x02, 0x21, 0x02}},
    {"www_forward",    {0x92, 0x02, 0x25, 0x02}},
    {"www_back",       {0x92, 0x02, 0x24, 0x02}},
    {"www_stop",       {0x92, 0x02, 0x26, 0x02}},
    {"www_refresh",    {0x92, 0x02, 0x27, 0x02}},
    {"www_favorites",  {0x92, 0x02, 0x2a, 0x02}},
    {"favorites",      {0x92, 0x02, 0x2a, 0x02}},  // alias
};

// -----------------------------------------------------------------------
// Keyboard modifier bit flags (byte 1 of the action)
// USB HID modifier byte: bit 0=LCtrl, 1=LShift, 2=LAlt, 3=LMeta,
//                        4=RCtrl, 5=RShift, 6=RAlt, 7=RMeta
// -----------------------------------------------------------------------
static const std::map<std::string, uint8_t> modifier_bits = {
    {"ctrl_l",  0x01},
    {"shift_l", 0x02},
    {"alt_l",   0x04},
    {"super_l", 0x08},
    {"meta_l",  0x08},
    {"ctrl_r",  0x10},
    {"shift_r", 0x20},
    {"alt_r",   0x40},
    {"super_r", 0x80},
    {"meta_r",  0x80},
    // Aliases without the _l/_r suffix default to left variant
    {"ctrl",    0x01},
    {"shift",   0x02},
    {"alt",     0x04},
    {"super",   0x08},
    {"meta",    0x08},
};

// -----------------------------------------------------------------------
// Keyboard key USB HID usage codes (byte 2 of the action)
// Reference: USB HID Usage Tables, Section 10 (Keyboard/Keypad)
// -----------------------------------------------------------------------
static const std::map<std::string, uint8_t> key_codes = {
    // Letters
    {"a", 0x04}, {"b", 0x05}, {"c", 0x06}, {"d", 0x07},
    {"e", 0x08}, {"f", 0x09}, {"g", 0x0a}, {"h", 0x0b},
    {"i", 0x0c}, {"j", 0x0d}, {"k", 0x0e}, {"l", 0x0f},
    {"m", 0x10}, {"n", 0x11}, {"o", 0x12}, {"p", 0x13},
    {"q", 0x14}, {"r", 0x15}, {"s", 0x16}, {"t", 0x17},
    {"u", 0x18}, {"v", 0x19}, {"w", 0x1a}, {"x", 0x1b},
    {"y", 0x1c}, {"z", 0x1d},
    // Numbers (top row)
    {"1", 0x1e}, {"2", 0x1f}, {"3", 0x20}, {"4", 0x21},
    {"5", 0x22}, {"6", 0x23}, {"7", 0x24}, {"8", 0x25},
    {"9", 0x26}, {"0", 0x27},
    // Common non-alpha keys
    {"enter",     0x28}, {"return",    0x28},
    {"escape",    0x29}, {"esc",       0x29},
    {"backspace", 0x2a},
    {"tab",       0x2b},
    {"space",     0x2c},
    {"minus",     0x2d}, {"-",         0x2d},
    {"equal",     0x2e}, {"=",         0x2e},
    {"lbracket",  0x2f}, {"[",         0x2f},
    {"rbracket",  0x30}, {"]",         0x30},
    {"backslash", 0x31}, {"\\",        0x31},
    {"semicolon", 0x33}, {";",         0x33},
    {"quote",     0x34}, {"'",         0x34},
    {"grave",     0x35}, {"`",         0x35},
    {"comma",     0x36}, {",",         0x36},
    {"dot",       0x37}, {".",         0x37},
    {"slash",     0x38}, {"/",         0x38},
    {"capslock",  0x39},
    // Function keys
    {"f1",  0x3a}, {"f2",  0x3b}, {"f3",  0x3c}, {"f4",  0x3d},
    {"f5",  0x3e}, {"f6",  0x3f}, {"f7",  0x40}, {"f8",  0x41},
    {"f9",  0x42}, {"f10", 0x43}, {"f11", 0x44}, {"f12", 0x45},
    {"f13", 0x68}, {"f14", 0x69}, {"f15", 0x6a}, {"f16", 0x6b},
    {"f17", 0x6c}, {"f18", 0x6d}, {"f19", 0x6e}, {"f20", 0x6f},
    {"f21", 0x70}, {"f22", 0x71}, {"f23", 0x72}, {"f24", 0x73},
    // Navigation
    {"printscreen", 0x46},
    {"scrolllock",  0x47},
    {"pause",       0x48},
    {"insert",      0x49},
    {"home",        0x4a},
    {"pageup",      0x4b},
    {"delete",      0x4c},
    {"end",         0x4d},
    {"pagedown",    0x4e},
    {"right",       0x4f},
    {"left",        0x50},
    {"down",        0x51},
    {"up",          0x52},
    // Numpad
    {"num0", 0x62}, {"num1", 0x59}, {"num2", 0x5a}, {"num3", 0x5b},
    {"num4", 0x5c}, {"num5", 0x5d}, {"num6", 0x5e}, {"num7", 0x5f},
    {"num8", 0x60}, {"num9", 0x61},
    {"numenter", 0x58}, {"numdot", 0x63},
    {"numplus",  0x57}, {"numminus", 0x56},
    {"nummul",   0x55}, {"numdiv",   0x54},
    {"numlock",  0x53},
};

// -----------------------------------------------------------------------
// parse_action
// -----------------------------------------------------------------------

// Split a string by a delimiter
static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::istringstream ss(s);
    std::string part;
    while (std::getline(ss, part, delim))
        if (!part.empty()) parts.push_back(part);
    return parts;
}

// Convert a string to lowercase
static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

bool parse_action(const std::string& action_raw, ActionBytes& out) {
    std::string action = to_lower(action_raw);

    // 1. Check for fire button with parameters: "fire:speed:times"
    if (action.substr(0, 5) == "fire:") {
        auto parts = split(action, ':');
        if (parts.size() == 3) {
            try {
                int speed = std::stoi(parts[1]);
                int times = std::stoi(parts[2]);
                if (speed >= 3 && speed <= 255 && times >= 0 && times <= 3) {
                    uint8_t checksum = (0x55u - (0x04u + speed + times)) & 0xFF;
                    out = {0x04, static_cast<uint8_t>(speed), static_cast<uint8_t>(times), checksum};
                    return true;
                }
            } catch (...) {}
        }
        return false;
    }

    // 2. Try direct mouse/special action lookup
    auto it = mouse_actions.find(action);
    if (it != mouse_actions.end()) {
        out = it->second;
        return true;
    }

    // 3. Treat as keyboard action, possibly with modifier prefix(es)
    // Format: [mod+]*key  e.g. "ctrl_l+shift_l+z" or "a+b+c" (multi-key)
    auto parts = split(action, '+');
    if (parts.empty()) return false;
    
    uint8_t mods = 0x00;
    std::vector<uint8_t> keys;

    for (auto& part : parts) {
        auto mit = modifier_bits.find(part);
        if (mit != modifier_bits.end()) {
            mods |= mit->second;
        } else {
            // Must be a key
            auto kit = key_codes.find(part);
            if (kit == key_codes.end()) return false;
            keys.push_back(kit->second);
        }
    }

    // Handle different cases:
    if (keys.empty()) {
        // Modifier-only binding (e.g. just "ctrl_l")
        out = {0x90, mods, 0x00, 0x00};
        return true;
    } else if (keys.size() == 1) {
        // Single key with optional modifiers
        out = {0x90, mods, keys[0], 0x00};
        return true;
    } else {
        // Multi-key combination - encode key count in byte 3
        // Protocol.cpp will detect this and generate proper multi-key events
        if (keys.size() > 255) return false;  // too many keys
        out = {0x90, mods, keys[0], static_cast<uint8_t>(keys.size())};
        return true;
    }
}

bool parse_multikey(const std::string& action, uint8_t& mods, std::vector<uint8_t>& keys) {
    std::string lower_action = to_lower(action);
    auto parts = split(lower_action, '+');
    if (parts.empty()) return false;

    mods = 0x00;
    keys.clear();

    for (auto& part : parts) {
        auto mit = modifier_bits.find(part);
        if (mit != modifier_bits.end()) {
            mods |= mit->second;
        } else {
            auto kit = key_codes.find(part);
            if (kit == key_codes.end()) return false;
            keys.push_back(kit->second);
        }
    }
    return true;
}

void list_actions() {
    std::cout << "Mouse/special actions:\n";
    for (auto& [name, _] : mouse_actions)
        std::cout << "  " << name << "\n";

    std::cout << "\nModifier keys (combine with + before a key):\n";
    std::cout << "  ctrl_l, shift_l, alt_l, super_l, ctrl_r, shift_r, alt_r, super_r\n";
    std::cout << "  (aliases: ctrl, shift, alt, super, meta)\n";

    std::cout << "\nKeyboard keys:\n  ";
    int col = 0;
    for (auto& [name, _] : key_codes) {
        std::cout << name;
        if (++col % 10 == 0) std::cout << "\n  ";
        else std::cout << " ";
    }
    std::cout << "\n";

    std::cout << "\nExample combos:\n"
              << "  ctrl_l+c          (copy)\n"
              << "  ctrl_l+shift_l+z  (redo)\n"
              << "  alt_l+f4          (close window)\n"
              << "  f5                (reload)\n";
}