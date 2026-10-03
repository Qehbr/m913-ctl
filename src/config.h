#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "protocol.h"

// Parsed representation of an INI configuration file.
// All values are stored as strings and validated when applied.
struct Config {
    // [dpi] section
    struct DpiConfig {
        uint16_t value   = 0;            // 0 means "not set"
        bool     enabled = true;
        uint32_t color   = 0xFFFFFFFF;   // per-slot LED color (Compx); 0xFFFFFFFF = not set
    };
    DpiConfig dpi[DPI_SLOTS];  // dpi[0] = dpi1, ..., dpi[4] = dpi5

    // [buttons] section: button name → action string
    std::map<std::string, std::string> buttons;

    // [macros] section: button name → macro spec (see parse_macro_spec).
    //
    // Keyed the same way as `buttons`, with the "button_" prefix, because a
    // macro belongs to a button: the hardware has one macro region per button
    // and no way to name or share them. Defining a macro for a button also
    // binds that button to it, so listing the same button in both sections is
    // a conflict rather than a combination.
    std::map<std::string, std::string> macros;

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

// Validate a parsed Config and throw std::runtime_error if any value is out of
// range. is_compx selects the DPI rules: the two revisions accept different
// value sets, so this can only run once the device has been identified.
void validate_config(const Config& cfg, bool is_compx);

// Map INI button names to Button enum values.
// Returns false if the name is not recognized.
bool parse_button_name(const std::string& name, Button& out);

// Canonical INI key for a button ("button_side1", "button_left") — the
// inverse of parse_button_name(), used when writing a config back out. Only
// the friendly spellings are produced; the mouse_m908 "button_1" names still
// parse but are not emitted.
std::string button_ini_name(Button b);

// The 16 buttons in the order a written-out config lists them: the four
// named buttons first, then side1..side12, rather than the protocol's
// interleaved index order.
const std::array<Button, 16>& button_ini_order();

// One named packet sequence, as written to the device in order.
struct ConfigSequence {
    std::string         label;
    std::vector<Packet> packets;
};

// Everything a Config translates into on the wire, in the order it has to be
// written. Pure — no I/O — so the round-trip check in tests/regress.sh can
// build the exact byte stream the tool would send, apply it to a simulated
// device memory, and hand that to decode_device_config() to confirm the write
// and read halves agree. main() only sends what this returns.
//
// Entries whose button name or action is unrecognised are skipped: they
// cannot occur through main(), which runs validate_config() first, and that
// throws on them.
std::vector<ConfigSequence> build_config_sequences(const Config& cfg,
                                                   const uint8_t* layout,
                                                   bool is_compx);

// Serialise a Config as INI text that parse_config_file() accepts back.
//
// `header` is emitted as leading comment lines (provenance of a --save dump);
// pass "" for none. `commented` holds entries to write as commented-out lines
// — how --save reports a button whose stored bytes it could not name, so the
// file stays applicable while still showing what was there.
std::string config_to_ini(const Config& cfg,
                          const std::string& header = "",
                          const std::map<std::string, std::string>& commented = {});
