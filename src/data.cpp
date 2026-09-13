#include "data.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <set>
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
    // "fire" = rapid fire: one press sends a burst of N left clicks. NOT a
    // repeat-while-held autoclicker, and N maxes out at 3 in firmware --
    // measured on 25a7:fa07, times=4+ is stored verbatim but fires nothing at
    // all, so the times<=3 check in parse_action() is load-bearing. That same
    // measurement is what lets times=0 mean "no clicks"; see parse_action().
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
    // Arrow keys. "left" and "right" are also mouse-button action names, and
    // mouse actions are matched first, so those two are only reachable inside
    // a combo ("ctrl+left"). The arrow_* aliases make them bindable on their
    // own; up/down need no alias but are given one for symmetry.
    {"right",       0x4f}, {"arrow_right", 0x4f},
    {"left",        0x50}, {"arrow_left",  0x50},
    {"down",        0x51}, {"arrow_down",  0x51},
    {"up",          0x52}, {"arrow_up",    0x52},
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
// Alias sets — which spellings are NOT canonical
//
// Several names above share one encoding. Parsing accepts them all; the
// reverse direction (action_name(), decode_key_event_list()) must pick
// exactly one spelling per encoding, or a config read back off the mouse
// would not necessarily parse back to the bytes it came from.
//
// These sets name the losers. Two of the choices are not cosmetic:
//
//   * "left"/"right" are mouse-button action names AND arrow keycodes. If
//     the reverse direction emitted "left" for keycode 0x50, re-applying the
//     INI would bind a mouse click instead of the arrow key. It must emit
//     "arrow_left". ("up"/"down" have no such clash but follow suit, so the
//     four arrows read the same way in a saved file.)
//   * the one-character punctuation names ("-", "=", "[") are legal in an
//     INI value but read as noise; the word forms are emitted instead.
//
// tests/regress.sh round-trips every name in the tables above through the
// reverse direction, so a new alias that is not listed here fails the suite
// rather than silently corrupting a saved config.
// -----------------------------------------------------------------------
static const std::set<std::string> action_aliases = {
    "dpi-loop",   // = dpi-cycle
    "rgb_toggle", // = led_toggle
    "disable",    // = none
    "favorites",  // = www_favorites
};

static const std::set<std::string> modifier_aliases = {
    "ctrl_l", "shift_l", "alt_l", "super_l",  // = ctrl, shift, alt, super
    "meta_l", "meta",                         // = super
    "meta_r",                                 // = super_r
};

static const std::set<std::string> key_aliases = {
    "return", "escape",                              // = enter, esc
    "left", "right", "up", "down",                   // = arrow_* (see above)
    // punctuation: the word forms (minus, equal, lbracket, …) are canonical
    "-", "=", "[", "]", "\\", ";", "'", "`", ",", ".", "/",
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
                // times<=3 is a measured hardware ceiling, not caution: the
                // mouse stores 4+ but then fires nothing. See the note on the
                // "fire" entry above before touching this bound.
                if (speed >= 3 && speed <= 255 && times >= 0 && times <= 3) {
                    // Measured click counts on 25a7:fa07:
                    //   times on wire | 0 | 1 | 2 | 3 | 4+
                    //   clicks fired  | 1 | 1 | 2 | 3 | 0
                    //
                    // So the firmware has no encoding for "no clicks" below 4,
                    // and 0 is an alias for 1. Asking for times=0 and getting
                    // one click is the opposite of what the config says, so
                    // spell 0 as 4 on the wire -- that is the value that really
                    // fires nothing. 1..3 are literal and go through unchanged.
                    int wire_times = (times == 0) ? 4 : times;
                    uint8_t checksum = (0x55u - (0x04u + speed + wire_times)) & 0xFF;
                    out = {0x04, static_cast<uint8_t>(speed),
                           static_cast<uint8_t>(wire_times), checksum};
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

size_t action_combo_tokens(const ActionBytes& action) {
    // Only keyboard bindings (0x90) are encoded as HID event lists; every
    // other action type is a fixed 4-byte code with no capacity limit.
    if (action[0] != 0x90) return 0;

    size_t tokens = 0;
    for (uint8_t bits = action[1]; bits; bits &= bits - 1)
        ++tokens;                                  // one per set modifier bit

    if (action[3] > 1)            tokens += action[3];  // multi-key: byte 3 = key count
    else if (action[2] != 0x00)   tokens += 1;          // single key

    return tokens;
}

// -----------------------------------------------------------------------
// Reverse direction: bytes → names (used by --save)
// -----------------------------------------------------------------------

// Reverse lookups, built once from the forward tables minus the alias sets,
// so there is still only one place where a name and its encoding are paired.
static const std::map<ActionBytes, std::string>& action_by_bytes() {
    static const std::map<ActionBytes, std::string> m = [] {
        std::map<ActionBytes, std::string> t;
        for (auto& [name, ab] : mouse_actions)
            if (!action_aliases.count(name)) t.emplace(ab, name);
        return t;
    }();
    return m;
}

static const std::map<uint8_t, std::string>& key_by_code() {
    static const std::map<uint8_t, std::string> m = [] {
        std::map<uint8_t, std::string> t;
        for (auto& [name, code] : key_codes)
            if (!key_aliases.count(name)) t.emplace(code, name);
        return t;
    }();
    return m;
}

static const std::map<uint8_t, std::string>& modifier_by_bit() {
    static const std::map<uint8_t, std::string> m = [] {
        std::map<uint8_t, std::string> t;
        for (auto& [name, bit] : modifier_bits)
            if (!modifier_aliases.count(name)) t.emplace(bit, name);
        return t;
    }();
    return m;
}

std::string action_name(const ActionBytes& action) {
    // Exact table match first. This is what keeps "fire" and "three_click"
    // readable: both are 0x04 actions and three_click's bytes are exactly
    // what "fire:50:3" encodes to, so the two are indistinguishable on the
    // wire. Whichever name the table gives re-encodes to the same bytes, so
    // either spelling round-trips — the table's is just the friendlier one.
    auto it = action_by_bytes().find(action);
    if (it != action_by_bytes().end()) return it->second;

    // Rapid fire with non-default parameters: 0x04, speed, times, checksum.
    if (action[0] == 0x04) {
        uint8_t speed = action[1];
        uint8_t times = action[2];
        uint8_t want  = static_cast<uint8_t>((0x55u - (0x04u + speed + times)) & 0xFF);
        if (action[3] != want) return "";   // not a fire action after all
        if (speed < 3) return "";           // parse_action would reject it
        // 4 on the wire is how parse_action() spells "no clicks"; anything
        // above 3 is a value the firmware stores and then ignores, so it has
        // no name to give back.
        if (times == 4)      return "fire:" + std::to_string(speed) + ":0";
        if (times >= 1 && times <= 3)
            return "fire:" + std::to_string(speed) + ":" + std::to_string(times);
        return "";
    }

    return "";
}

std::string decode_key_event_list(const uint8_t* p, size_t n) {
    if (n < 2) return "";

    size_t count = p[0];
    // Layout: count, then count × 3 event bytes, then a 1-byte inner
    // checksum. An erased slot reads 0xFF, which fails this immediately.
    if (count == 0 || 1 + count * 3 + 1 > n) return "";

    uint16_t sum = static_cast<uint16_t>(count);
    for (size_t i = 0; i < count * 3; ++i) sum += p[1 + i];
    uint8_t inner = static_cast<uint8_t>((0x55u - (sum & 0xFF)) & 0xFF);
    if (p[1 + count * 3] != inner) return "";

    uint8_t              mods = 0;
    std::vector<uint8_t> keys;
    std::string          consumer;

    for (size_t i = 0; i < count; ++i) {
        uint8_t type  = p[1 + i * 3];
        uint8_t value = p[2 + i * 3];
        uint8_t extra = p[3 + i * 3];
        switch (type) {
        case 0x80: mods |= value;       break;  // modifier down
        case 0x81: keys.push_back(value); break; // key down
        case 0x82: {                             // consumer / multimedia down
            // Stored as {0x92, extra, code, extra} by parse_action, and the
            // event carries [type][code][extra] — so match on both.
            for (auto& [name, ab] : mouse_actions) {
                if (ab[0] == 0x92 && ab[2] == value && ab[1] == extra &&
                    !action_aliases.count(name)) {
                    consumer = name;
                    break;
                }
            }
            if (consumer.empty()) return "";
            break;
        }
        case 0x40: case 0x41: case 0x42:
            break;                               // the matching up events
        default:
            return "";                           // unknown event type
        }
    }

    // A consumer binding is a whole action on its own; mixing it with keys is
    // not something parse_action() can express, so refuse rather than guess.
    if (!consumer.empty())
        return (mods == 0 && keys.empty()) ? consumer : "";

    std::vector<std::string> tokens;
    for (uint8_t bit = 0x01; bit; bit = static_cast<uint8_t>(bit << 1)) {
        if (!(mods & bit)) continue;
        auto mit = modifier_by_bit().find(bit);
        if (mit == modifier_by_bit().end()) return "";
        tokens.push_back(mit->second);
        if (bit == 0x80) break;   // avoid wrapping to 0 on the last shift
    }
    for (uint8_t k : keys) {
        auto kit = key_by_code().find(k);
        if (kit == key_by_code().end()) return "";
        tokens.push_back(kit->second);
    }
    if (tokens.empty()) return "";

    std::string out = tokens[0];
    for (size_t i = 1; i < tokens.size(); ++i) out += "+" + tokens[i];
    return out;
}

std::vector<std::string> all_parseable_action_names() {
    std::vector<std::string> names;
    for (auto& [name, _] : mouse_actions)  names.push_back(name);
    for (auto& [name, _] : key_codes)      names.push_back(name);
    for (auto& [name, _] : modifier_bits)  names.push_back(name);
    return names;
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
