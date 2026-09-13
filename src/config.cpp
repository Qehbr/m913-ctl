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

// Remove a trailing inline comment: whitespace followed by ';' or '#'.
//
// Whole-line comments were already handled, but inline ones were not, so
// "dpi2_enable=0   ; collapse to one stage" kept the comment as part of the
// value. That threw a clear error for keys parsed as numbers or actions, but
// dpiN_enable is tested with (value != "0") — so the comment silently made it
// true, i.e. the exact opposite of what the line said. README documented that
// form, so anyone copying it got five DPI stages while believing they had one.
//
// The leading-whitespace requirement is what makes this safe for '#':
// color=#ff0000 is valid syntax (parse_color strips the '#'), and only a
// space-prefixed '#' is treated as a comment. This matches how most INI
// parsers behave.
static std::string strip_inline_comment(const std::string& s) {
    for (size_t i = 1; i < s.size(); ++i)
        if ((s[i] == ';' || s[i] == '#') &&
            (s[i - 1] == ' ' || s[i - 1] == '\t'))
            return s.substr(0, i);
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

std::string button_ini_name(Button b) {
    switch (b) {
    case Button::Left:   return "button_left";
    case Button::Right:  return "button_right";
    case Button::Middle: return "button_middle";
    case Button::Fire:   return "button_fire";
    case Button::Side1:  return "button_side1";
    case Button::Side2:  return "button_side2";
    case Button::Side3:  return "button_side3";
    case Button::Side4:  return "button_side4";
    case Button::Side5:  return "button_side5";
    case Button::Side6:  return "button_side6";
    case Button::Side7:  return "button_side7";
    case Button::Side8:  return "button_side8";
    case Button::Side9:  return "button_side9";
    case Button::Side10: return "button_side10";
    case Button::Side11: return "button_side11";
    case Button::Side12: return "button_side12";
    }
    return "";
}

const std::array<Button, 16>& button_ini_order() {
    static const std::array<Button, 16> order = {
        Button::Left,  Button::Right,  Button::Middle, Button::Fire,
        Button::Side1, Button::Side2,  Button::Side3,  Button::Side4,
        Button::Side5, Button::Side6,  Button::Side7,  Button::Side8,
        Button::Side9, Button::Side10, Button::Side11, Button::Side12,
    };
    return order;
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
    for (int i = 0; i < DPI_SLOTS; ++i) {
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

        // Skip blank lines and whole-line comments
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        line = trim(strip_inline_comment(line));
        if (line.empty())
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
                // dpiN=VALUE or dpiN_enable=0/1 or dpiN_color=RRGGBB
                std::smatch dm;
                std::regex re_dpi_val(R"(^dpi([1-5])$)");
                std::regex re_dpi_ena(R"(^dpi([1-5])_enable$)");
                std::regex re_dpi_col(R"(^dpi([1-5])_color$)");

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
                } else if (std::regex_match(key, dm, re_dpi_col)) {
                    int slot = std::stoi(dm[1].str()) - 1;
                    if (!parse_color(value, cfg.dpi[slot].color))
                        throw std::runtime_error(
                            "Invalid color '" + value + "' at line " +
                            std::to_string(lineno));
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
// Config → packet sequences
// -----------------------------------------------------------------------

std::vector<ConfigSequence> build_config_sequences(const Config& cfg,
                                                   const uint8_t* layout,
                                                   bool is_compx) {
    std::vector<ConfigSequence> out;

    // ---- Buttons ----
    std::map<uint8_t, ActionBytes> btn_changes;
    for (auto& [key, action_str] : cfg.buttons) {
        Button btn;
        if (!parse_button_name(key, btn)) continue;
        ActionBytes ab;
        if (!parse_action(action_str, ab)) continue;
        btn_changes[static_cast<uint8_t>(btn)] = ab;
        // Multi-key combos cannot be rebuilt from the 4 action bytes alone
        // (only the first keycode and a count fit), so the builder re-parses
        // the original string; hand it over here.
        if (ab[0] == 0x90 && ab[3] > 1)
            register_multikey_action(static_cast<uint8_t>(btn), action_str);
    }
    if (!btn_changes.empty())
        out.push_back({"Button mapping", build_button_mapping(btn_changes, layout)});

    // ---- DPI ----
    // A dpiN_enable flag is worth sending on its own for Compx: that path
    // emits one packet per set DPI value plus a standalone stage-count
    // packet, so a stage change travels without touching any value. Areson
    // packs values and stage count into a single template-based sequence, so
    // sending it with no values would overwrite every slot with the
    // template's defaults — there the flags can only ride along with a value.
    //
    // This also keeps the LED block below honest: the stage count it derives
    // from enabled[] is only true of the device once those flags have been
    // sent. With Compx now always sending them, the two cannot disagree.
    bool any_dpi_value = false, any_dpi_disabled = false;
    for (int i = 0; i < DPI_SLOTS; ++i) {
        if (cfg.dpi[i].value != 0) any_dpi_value    = true;
        if (!cfg.dpi[i].enabled)   any_dpi_disabled = true;
    }

    if (any_dpi_value || (is_compx && any_dpi_disabled)) {
        DpiSettings dpi;
        for (int i = 0; i < DPI_SLOTS; ++i) {
            dpi.values[i]  = cfg.dpi[i].value;
            dpi.enabled[i] = cfg.dpi[i].enabled;
        }
        out.push_back({"DPI config", is_compx ? build_compx_dpi_packets(dpi)
                                              : build_dpi_packets(dpi)});
    }

    // ---- LED ----
    if (is_compx) {
        // Compx has per-slot RGB colors, no global LED modes.
        //   [led] section    → applies one color to every active slot
        //                       (mode=off → black)
        //   dpiN_color keys  → override individual slots, take precedence
        //
        // The slot count comes from enabled[], which for a run that set no
        // DPI options at all is every slot true — so such a run colours all
        // five stages. That is the right default: the active count cannot be
        // read back, and missing an active stage would leave it lit with its
        // old colour, very visible for `--led off`, whereas colouring an
        // inactive one does nothing.
        bool any_color = cfg.led.set;
        for (int i = 0; i < DPI_SLOTS; ++i)
            if (cfg.dpi[i].color != 0xFFFFFFFF) any_color = true;

        if (any_color) {
            std::array<bool, DPI_SLOTS> enabled_bits;
            for (int i = 0; i < DPI_SLOTS; ++i)
                enabled_bits[i] = cfg.dpi[i].enabled;
            int n_slots = compx_active_dpi_stage_count(enabled_bits);

            uint32_t colors[DPI_SLOTS];
            uint32_t global = cfg.led.set
                ? ((cfg.led.mode == LedMode::Off) ? 0x000000 : cfg.led.color)
                : 0xFFFFFFFF;
            for (int i = 0; i < DPI_SLOTS; ++i)
                colors[i] = (cfg.dpi[i].color != 0xFFFFFFFF) ? cfg.dpi[i].color : global;

            out.push_back({"LED color", build_compx_color_packets(colors, n_slots)});
        }
    } else if (cfg.led.set) {
        out.push_back({"LED mode", build_led_packets(cfg.led.mode, cfg.led.color,
                                                     cfg.led.brightness, cfg.led.speed)});
    }

    // ---- Polling rate ----
    if (cfg.mouse.set)
        out.push_back({"Polling rate", {build_polling_rate_packet(cfg.mouse.polling_rate)}});

    return out;
}

// -----------------------------------------------------------------------
// INI writer
// -----------------------------------------------------------------------

static std::string led_mode_name(LedMode m) {
    switch (m) {
    case LedMode::Off:         return "off";
    case LedMode::Steady:      return "steady";
    case LedMode::Respiration: return "respiration";
    case LedMode::Rainbow:     return "rainbow";
    }
    return "off";
}

// Index of the newline ending the line starting at `start`, or the string
// length for the last line.
static size_t header_line_end(const std::string& s, size_t start) {
    size_t nl = s.find('\n', start);
    return (nl == std::string::npos) ? s.size() : nl;
}

static std::string hex6(uint32_t rgb) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (int shift = 20; shift >= 0; shift -= 4)
        s += d[(rgb >> shift) & 0xF];
    return s;
}

std::string config_to_ini(const Config& cfg,
                          const std::string& header,
                          const std::map<std::string, std::string>& commented) {
    std::string out;

    if (!header.empty()) {
        // Each line becomes a comment. A trailing newline is not a line of its
        // own, so callers can end the header with one without getting a stray
        // ";" at the bottom.
        std::string h = header;
        if (h.back() == '\n') h.pop_back();
        size_t start = 0;
        for (;;) {
            size_t nl = header_line_end(h, start);
            std::string ln = h.substr(start, nl - start);
            out += ln.empty() ? ";\n" : "; " + ln + "\n";
            if (nl == h.size()) break;
            start = nl + 1;
        }
        out += "\n";
    }

    if (cfg.mouse.set)
        out += "[mouse]\npolling_rate=" +
               std::to_string(cfg.mouse.polling_rate) + "\n\n";

    bool any_dpi = false;
    for (int i = 0; i < DPI_SLOTS; ++i)
        if (cfg.dpi[i].value != 0 || !cfg.dpi[i].enabled ||
            cfg.dpi[i].color != 0xFFFFFFFF) any_dpi = true;
    if (any_dpi) {
        out += "[dpi]\n";
        for (int i = 0; i < DPI_SLOTS; ++i) {
            std::string n = std::to_string(i + 1);
            if (cfg.dpi[i].value != 0)
                out += "dpi" + n + "=" + std::to_string(cfg.dpi[i].value) + "\n";
            if (cfg.dpi[i].color != 0xFFFFFFFF)
                out += "dpi" + n + "_color=" + hex6(cfg.dpi[i].color) + "\n";
            // Only the first disabled slot is meaningful — stages cascade off
            // from there — but writing every one keeps the file honest about
            // what the device reported.
            if (!cfg.dpi[i].enabled)
                out += "dpi" + n + "_enable=0\n";
        }
        out += "\n";
    }

    if (cfg.led.set) {
        out += "[led]\nmode=" + led_mode_name(cfg.led.mode) + "\n";
        // Only the keys the mode actually uses: rainbow ignores the stored
        // colour and steady ignores speed, so emitting them would invite
        // someone to edit a value that does nothing.
        if (cfg.led.mode == LedMode::Steady || cfg.led.mode == LedMode::Respiration)
            out += "color=" + hex6(cfg.led.color) + "\n";
        if (cfg.led.mode != LedMode::Off)
            out += "brightness=" + std::to_string(cfg.led.brightness) + "\n";
        if (cfg.led.mode == LedMode::Respiration || cfg.led.mode == LedMode::Rainbow)
            out += "speed=" + std::to_string(cfg.led.speed) + "\n";
        out += "\n";
    }

    if (!cfg.buttons.empty() || !commented.empty()) {
        out += "[buttons]\n";
        for (Button b : button_ini_order()) {
            std::string key = button_ini_name(b);
            auto it = cfg.buttons.find(key);
            if (it != cfg.buttons.end()) {
                out += key + "=" + it->second + "\n";
                continue;
            }
            auto cit = commented.find(key);
            if (cit != commented.end())
                out += "; " + key + "=" + cit->second + "\n";
        }
        // Anything under a name outside the canonical 16 (a hand-written
        // "button_1" style key) still gets written out rather than dropped.
        for (auto& [key, action] : cfg.buttons) {
            Button b;
            if (parse_button_name(key, b) && button_ini_name(b) == key) continue;
            out += key + "=" + action + "\n";
        }
        out += "\n";
    }

    return out;
}

// -----------------------------------------------------------------------
// Validation
// -----------------------------------------------------------------------

void validate_config(const Config& cfg, bool is_compx) {
    if (cfg.mouse.set) {
        uint16_t r = cfg.mouse.polling_rate;
        if (r != 125 && r != 250 && r != 500 && r != 1000)
            throw std::runtime_error(
                "polling_rate must be 125, 250, 500, or 1000 (got " +
                std::to_string(r) + ")");
    }

    for (int i = 0; i < DPI_SLOTS; ++i) {
        uint16_t v = cfg.dpi[i].value;
        if (v == 0) continue;  // not configured, skip
        if (!dpi_value_supported(v, is_compx))
            throw std::runtime_error(
                "DPI" + std::to_string(i + 1) + " value " + std::to_string(v) +
                " is not supported by this " +
                (is_compx ? "(Compx)" : "(Areson)") + " hardware — nearest "
                "supported value is " +
                std::to_string(nearest_supported_dpi(v, is_compx)));
    }

    for (auto& [key, action] : cfg.buttons) {
        Button btn;
        if (!parse_button_name(key, btn))
            throw std::runtime_error("Unknown button name: " + key);

        ActionBytes ab;
        if (!parse_action(action, ab))
            throw std::runtime_error(
                "Unknown action '" + action + "' for button " + key);

        size_t tokens = action_combo_tokens(ab);
        if (tokens > MAX_COMBO_TOKENS)
            throw std::runtime_error(
                "Action '" + action + "' for button " + key + " combines " +
                std::to_string(tokens) + " modifiers+keys — the mouse stores at "
                "most " + std::to_string(MAX_COMBO_TOKENS) + " per binding");
    }
}