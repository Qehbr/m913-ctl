#include "config.h"
#include "data.h"

#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t start = s.find_first_not_of(ws);
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

static std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// -----------------------------------------------------------------------
// Button name → Button enum
// -----------------------------------------------------------------------

bool parse_button_name(const std::string& name, Button& out) {
    // Mapping uses mouse_m908 _c_button_names indices (0-15).
    // "button_1..6"  = 12 side buttons (indices 0..5)
    // "button_right" = index 6, "button_left" = index 7
    // "button_7..12" = more side buttons (indices 8..9, 12..15)
    // "button_middle"= index 10, "button_fire" = index 11
    static const std::map<std::string, Button> table = {
        // mouse_m908 canonical names
        {"button_1",      Button::Side1 },
        {"button_2",      Button::Side2 },
        {"button_3",      Button::Side3 },
        {"button_4",      Button::Side4 },
        {"button_5",      Button::Side5 },
        {"button_6",      Button::Side6 },
        {"button_right",  Button::Right },
        {"button_left",   Button::Left  },
        {"button_7",      Button::Side7 },
        {"button_8",      Button::Side8 },
        {"button_middle", Button::Middle},
        {"button_fire",   Button::Fire  },
        {"button_9",      Button::Side9 },
        {"button_10",     Button::Side10},
        {"button_11",     Button::Side11},
        {"button_12",     Button::Side12},
        // Friendly aliases (side1..12 map to button_1..12 via mouse_m908 numbering)
        {"button_side1",  Button::Side1 },
        {"button_side2",  Button::Side2 },
        {"button_side3",  Button::Side3 },
        {"button_side4",  Button::Side4 },
        {"button_side5",  Button::Side5 },
        {"button_side6",  Button::Side6 },
        {"button_side7",  Button::Side7 },
        {"button_side8",  Button::Side8 },
        {"button_side9",  Button::Side9 },
        {"button_side10", Button::Side10},
        {"button_side11", Button::Side11},
        {"button_side12", Button::Side12},
    };

    auto it = table.find(to_lower(name));
    if (it == table.end()) return false;
    out = it->second;
    return true;
}

// -----------------------------------------------------------------------
// LED mode string → LedMode enum
// -----------------------------------------------------------------------

static bool parse_led_mode(const std::string& s, LedMode& out) {
    std::string sl = to_lower(s);
    if (sl == "off")       { out = LedMode::Off;       return true; }
    if (sl == "static")    { out = LedMode::Steady;     return true; }
    if (sl == "steady")    { out = LedMode::Steady;     return true; }
    if (sl == "breathing") { out = LedMode::Respiration; return true; }
    if (sl == "respiration") { out = LedMode::Respiration; return true; }
    if (sl == "rainbow")   { out = LedMode::Rainbow;   return true; }
    return false;
}

// -----------------------------------------------------------------------
// Hex color string → uint32_t
// -----------------------------------------------------------------------

static bool parse_color(const std::string& s, uint32_t& out) {
    std::string hex = s;
    if (hex.size() > 0 && hex[0] == '#') hex = hex.substr(1);
    if (hex.size() != 6) return false;
    try {
        out = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
        return true;
    } catch (...) {
        return false;
    }
}

// -----------------------------------------------------------------------
// INI parser
// -----------------------------------------------------------------------

Config parse_config_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open config file: " + path);

    Config cfg;
    std::string section;
    int lineno = 0;

    // Pre-initialize DPI slots so that missing entries keep defaults
    for (int i = 0; i < 5; ++i) {
        cfg.dpi[i].enabled = true;
        cfg.dpi[i].value   = 0;  // 0 = not configured
    }

    // Regex patterns
    std::regex re_section(R"(^\[([^\]]+)\])");
    std::regex re_kv(R"(^([^=]+)=(.*)$)");

    std::string line;
    while (std::getline(f, line)) {
        ++lineno;
        line = trim(line);

        // Skip blank lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        std::smatch m;

        // Section header
        if (std::regex_match(line, m, re_section)) {
            section = to_lower(trim(m[1].str()));
            continue;
        }

        // Key=value pair
        if (std::regex_match(line, m, re_kv)) {
            std::string key   = to_lower(trim(m[1].str()));
            std::string value = trim(m[2].str());

            if (section == "dpi") {
                // dpiN=VALUE or dpiN_enable=0/1
                std::smatch dm;
                std::regex re_dpi_val(R"(^dpi([1-5])$)");
                std::regex re_dpi_ena(R"(^dpi([1-5])_enable$)");

                if (std::regex_match(key, dm, re_dpi_val)) {
                    int slot = std::stoi(dm[1].str()) - 1;
                    try {
                        int v = std::stoi(value);
                        cfg.dpi[slot].value = static_cast<uint16_t>(v);
                    } catch (...) {
                        throw std::runtime_error(
                            "Invalid DPI value '" + value + "' at line " +
                            std::to_string(lineno));
                    }
                } else if (std::regex_match(key, dm, re_dpi_ena)) {
                    int slot = std::stoi(dm[1].str()) - 1;
                    cfg.dpi[slot].enabled = (value != "0");
                }
                // Unknown dpi keys are silently ignored

            } else if (section == "buttons") {
                cfg.buttons[key] = value;

            } else if (section == "mouse") {
                if (key == "polling_rate") {
                    try {
                        int r = std::stoi(value);
                        cfg.mouse.polling_rate = static_cast<uint16_t>(r);
                        cfg.mouse.set = true;
                    } catch (...) {
                        throw std::runtime_error(
                            "Invalid polling_rate '" + value + "' at line " +
                            std::to_string(lineno));
                    }
                }

            } else if (section == "led") {
                cfg.led.set = true;
                if (key == "mode") {
                    if (!parse_led_mode(value, cfg.led.mode))
                        throw std::runtime_error(
                            "Unknown LED mode '" + value + "' at line " +
                            std::to_string(lineno));
                } else if (key == "color") {
                    if (!parse_color(value, cfg.led.color))
                        throw std::runtime_error(
                            "Invalid color '" + value + "' at line " +
                            std::to_string(lineno));
                } else if (key == "brightness") {
                    try {
                        int b = std::stoi(value);
                        cfg.led.brightness = static_cast<uint8_t>(
                            std::max(0, std::min(255, b)));
                    } catch (...) {
                        throw std::runtime_error(
                            "Invalid brightness '" + value + "' at line " +
                            std::to_string(lineno));
                    }
                } else if (key == "speed") {
                    try {
                        int s = std::stoi(value);
                        cfg.led.speed = static_cast<uint8_t>(
                            std::max(1, std::min(5, s)));
                    } catch (...) {
                        throw std::runtime_error(
                            "Invalid speed '" + value + "' at line " +
                            std::to_string(lineno));
                    }
                }
            }
            // Unknown sections are silently ignored
        }
    }

    return cfg;
}

// -----------------------------------------------------------------------
// Validation
// -----------------------------------------------------------------------

void validate_config(const Config& cfg) {
    if (cfg.mouse.set) {
        uint16_t r = cfg.mouse.polling_rate;
        if (r != 125 && r != 250 && r != 500 && r != 1000)
            throw std::runtime_error(
                "polling_rate must be 125, 250, 500, or 1000 (got " +
                std::to_string(r) + ")");
    }

    for (int i = 0; i < 5; ++i) {
        uint16_t v = cfg.dpi[i].value;
        if (v == 0) continue;  // not configured, skip
        if (v < 100 || v > 16000 || v % 100 != 0)
            throw std::runtime_error(
                "DPI" + std::to_string(i + 1) + " value " + std::to_string(v) +
                " is out of range (100–16000 in steps of 100)");
    }

    for (auto& [key, action] : cfg.buttons) {
        Button btn;
        if (!parse_button_name(key, btn))
            throw std::runtime_error("Unknown button name: " + key);

        ActionBytes ab;
        if (!parse_action(action, ab))
            throw std::runtime_error(
                "Unknown action '" + action + "' for button " + key);
    }
}