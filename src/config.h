#pragma once

#include <map>
#include <string>
#include <vector>

#include "protocol.h"

// Parsed representation of an INI configuration file.
// All values are stored as strings and validated when applied.
struct Config {
    Profile profile = Profile::P1;

    // [dpi] section
    struct DpiConfig {
        uint16_t value   = 0;     // 0 means "not set"
        bool     enabled = true;
    };
    DpiConfig dpi[5];  // dpi[0] = dpi1, ..., dpi[4] = dpi5

    // [buttons] section: button name → action string
    std::map<std::string, std::string> buttons;

    // [led] section
    struct LedConfig {
        LedMode  mode       = LedMode::Rainbow;
        uint32_t color      = 0x00ff00;  // RGB
        uint8_t  brightness = 0xff;
        uint8_t  speed      = 0x03;      // 1-5 (respiration speed, 1=slow, 5=fast)
        bool     set        = false;     // true if [led] section was present
    } led;

    // [mouse] section
    struct MouseConfig {
        uint16_t polling_rate = 1000;   // Hz: 125, 250, 500, or 1000
        bool     set          = false;  // true if polling_rate was specified
    } mouse;
};

// Parse an INI config file from disk.
// Throws std::runtime_error if the file cannot be read or has syntax errors.
Config parse_config_file(const std::string& path);

// Validate a parsed Config and throw std::runtime_error if any value is out of range.
void validate_config(const Config& cfg);

// Map INI button names to Button enum values.
// Returns false if the name is not recognized.
bool parse_button_name(const std::string& name, Button& out);