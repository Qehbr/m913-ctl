#include "readback.h"

#include <iomanip>
#include <sstream>

#include "data.h"
#include "protocol.h"

// -----------------------------------------------------------------------
// Address map (Areson)
//
// These are the write-side addresses, read back. Each one is checked against
// the template it is written by, in protocol.cpp:
//   0x0000  polling rate      build_polling_rate_packet
//   0x0002  active DPI stages dpi_template[3]
//   0x000c  DPI slot 1, +4 per slot, 5 slots    dpi_template[0..2]
//   0x0054  LED colour r,g,b + inner checksum   led_static / led_breathing
//   0x0058  LED mode                            led_off writes just this
//   0x005a  LED brightness
//   0x005c  LED respiration/rainbow speed       led_breathing[1]
//   0x0060  button 1 action, +4 per button, 16  default_button_mapping
//   0x0100  keyboard event list, +0x20 per button, 16   kb_key_addr
// -----------------------------------------------------------------------
static constexpr uint16_t ADDR_POLLING    = 0x0000;
static constexpr uint16_t ADDR_STAGES     = 0x0002;
static constexpr uint16_t ADDR_DPI_BASE   = 0x000c;
static constexpr uint16_t DPI_STRIDE      = 0x0004;
static constexpr uint16_t ADDR_LED_COLOR  = 0x0054;
static constexpr uint16_t ADDR_LED_MODE   = 0x0058;
static constexpr uint16_t ADDR_LED_BRIGHT = 0x005a;
static constexpr uint16_t ADDR_LED_SPEED  = 0x005c;
static constexpr uint16_t ADDR_BTN_BASE   = 0x0060;
static constexpr uint16_t BTN_STRIDE      = 0x0004;
static constexpr uint16_t ADDR_KB_BASE    = 0x0100;
static constexpr uint16_t KB_STRIDE       = 0x0020;

// A keyboard binding is at most a count byte, 18 event bytes and an inner
// checksum — exactly what the two 10-byte chunks read per slot cover.
static constexpr size_t KB_SLOT_BYTES = 2 * READ_CHUNK;

// -----------------------------------------------------------------------
// Image access
// -----------------------------------------------------------------------

static bool byte_at(const BlockMap& blocks, uint16_t addr, uint8_t& out) {
    auto it = blocks.upper_bound(addr);
    if (it == blocks.begin()) return false;
    --it;
    if (addr - it->first >= static_cast<int>(READ_CHUNK)) return false;
    out = it->second[addr - it->first];
    return true;
}

// Fetch `n` consecutive bytes; false if any of them is missing.
static bool bytes_at(const BlockMap& blocks, uint16_t addr, size_t n, uint8_t* out) {
    for (size_t i = 0; i < n; ++i)
        if (!byte_at(blocks, static_cast<uint16_t>(addr + i), out[i])) return false;
    return true;
}

static std::string hex2(uint8_t v) {
    std::ostringstream s;
    s << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(v);
    return s.str();
}

// -----------------------------------------------------------------------
// Decoding
// -----------------------------------------------------------------------

bool ack_matches(const Packet& request, const uint8_t reply[M913_PACKET_SIZE]) {
    return reply[0] == 0x09 &&
           reply[1] == request[1] &&
           reply[3] == request[3] &&
           reply[4] == request[4];
}

bool verify_reply_checksum(const uint8_t pkt[M913_PACKET_SIZE]) {
    // Device → host formula, byte[0] (report ID) excluded — the host → device
    // direction uses a different base, see compute_checksum().
    uint16_t sum = 0;
    for (int i = 1; i < M913_PACKET_SIZE - 1; ++i) sum += pkt[i];
    return pkt[M913_PACKET_SIZE - 1] ==
           static_cast<uint8_t>((0x4Cu - sum) & 0xFF);
}

bool decode_device_config(const BlockMap& blocks, Config& out, DecodeReport& report) {
    if (blocks.empty()) {
        report.warnings.push_back("no blocks were read back");
        return false;
    }

    int decoded_fields = 0;
    uint8_t b = 0;

    // ---- polling rate ----
    if (byte_at(blocks, ADDR_POLLING, b)) {
        uint16_t hz = 0;
        switch (b) {
        case 0x01: hz = 1000; break;
        case 0x02: hz = 500;  break;
        case 0x04: hz = 250;  break;
        case 0x08: hz = 125;  break;
        default: break;
        }
        if (hz) {
            out.mouse.polling_rate = hz;
            out.mouse.set          = true;
            ++decoded_fields;
        } else {
            report.warnings.push_back("polling rate byte 0x" + hex2(b) +
                                      " is not a rate this tool knows");
        }
    } else {
        report.missing.push_back(ADDR_POLLING);
    }

    // ---- active DPI stage count ----
    // The device stores a count, not a per-slot bitmask, and stages cascade
    // off from the top: count = 3 means slots 4 and 5 are off. Writing it
    // back as dpi4_enable=0 reproduces exactly that, because
    // build_dpi_packets() derives the count from the first disabled slot.
    if (byte_at(blocks, ADDR_STAGES, b)) {
        if (b >= 1 && b <= DPI_SLOTS) {
            for (int i = 0; i < DPI_SLOTS; ++i)
                out.dpi[i].enabled = (i < b);
            ++decoded_fields;
        } else {
            report.warnings.push_back("active DPI stage count 0x" + hex2(b) +
                                      " is out of range 1-" +
                                      std::to_string(DPI_SLOTS));
        }
    } else {
        report.missing.push_back(ADDR_STAGES);
    }

    // ---- DPI values ----
    for (int i = 0; i < DPI_SLOTS; ++i) {
        uint16_t addr = static_cast<uint16_t>(ADDR_DPI_BASE + i * DPI_STRIDE);
        uint8_t  slot[DPI_STRIDE];
        if (!bytes_at(blocks, addr, DPI_STRIDE, slot)) {
            report.missing.push_back(addr);
            continue;
        }
        uint16_t dpi = dpi_from_code(slot[0], /*is_compx=*/false);
        if (dpi == 0 || slot[1] != slot[0]) {
            report.warnings.push_back(
                "DPI slot " + std::to_string(i + 1) + " holds " +
                hex2(slot[0]) + " " + hex2(slot[1]) +
                ", which is not a value this hardware's table encodes");
            continue;
        }
        out.dpi[i].value = dpi;
        ++decoded_fields;
    }

    // ---- LED ----
    uint8_t mode_byte = 0;
    if (byte_at(blocks, ADDR_LED_MODE, mode_byte)) {
        bool known = true;
        switch (mode_byte) {
        case 0x00: out.led.mode = LedMode::Off;         break;
        case 0x01: out.led.mode = LedMode::Steady;      break;
        case 0x02: out.led.mode = LedMode::Respiration; break;
        case 0x03: out.led.mode = LedMode::Rainbow;     break;
        default:   known = false;                       break;
        }
        if (known) {
            out.led.set = true;
            ++decoded_fields;
        } else {
            report.warnings.push_back("LED mode byte 0x" + hex2(mode_byte) +
                                      " is not a mode this tool knows");
        }
    } else {
        report.missing.push_back(ADDR_LED_MODE);
    }

    uint8_t rgb[3];
    if (bytes_at(blocks, ADDR_LED_COLOR, 3, rgb))
        out.led.color = (static_cast<uint32_t>(rgb[0]) << 16) |
                        (static_cast<uint32_t>(rgb[1]) << 8) | rgb[2];
    else
        report.missing.push_back(ADDR_LED_COLOR);

    if (byte_at(blocks, ADDR_LED_BRIGHT, b))
        out.led.brightness = b;
    else
        report.missing.push_back(ADDR_LED_BRIGHT);

    if (byte_at(blocks, ADDR_LED_SPEED, b)) {
        if (b >= 1 && b <= 5) {
            out.led.speed = b;
        } else {
            // Only respiration and rainbow use this register; in the other
            // modes it holds whatever the last such mode left behind.
            report.warnings.push_back("LED speed byte 0x" + hex2(b) +
                                      " is outside 1-5; keeping the default");
        }
    } else {
        report.missing.push_back(ADDR_LED_SPEED);
    }

    // ---- buttons ----
    // On Areson the protocol index is the Button enum value (the identity
    // layout build_button_mapping() uses when no translation table is given),
    // so slot i is button i.
    for (int i = 0; i < 16; ++i) {
        Button      btn  = static_cast<Button>(i);
        std::string name = button_ini_name(btn);
        uint16_t    addr = static_cast<uint16_t>(ADDR_BTN_BASE + i * BTN_STRIDE);

        ActionBytes ab{};
        if (!bytes_at(blocks, addr, ab.size(), ab.data())) {
            report.missing.push_back(addr);
            continue;
        }

        if (ab == KB_LIST_MARKER) {
            // The action lives in this button's event list instead.
            uint16_t kb_addr = static_cast<uint16_t>(ADDR_KB_BASE + i * KB_STRIDE);
            uint8_t  list[KB_SLOT_BYTES];
            if (!bytes_at(blocks, kb_addr, KB_SLOT_BYTES, list)) {
                report.missing.push_back(kb_addr);
                continue;
            }
            std::string action = decode_key_event_list(list, KB_SLOT_BYTES);
            if (action.empty()) {
                std::string raw;
                for (size_t k = 0; k < KB_SLOT_BYTES; ++k)
                    raw += (k ? " " : "") + hex2(list[k]);
                report.unnamed_buttons[name] = "<keyboard list: " + raw + ">";
                report.warnings.push_back(
                    name + " points at a keyboard event list that could not "
                           "be decoded");
                continue;
            }
            out.buttons[name] = action;
            ++decoded_fields;
            continue;
        }

        if (is_macro_action(ab)) {
            // The binding is decodable but the macro itself is not: the read
            // codes do not cover the 0x0300+ regions, and nothing decodes the
            // event list yet. Say so rather than reporting unknown bytes for a
            // config this tool may well have written itself.
            std::string repeat = (ab[2] == MACRO_REPEAT_HOLD)   ? "hold"
                               : (ab[2] == MACRO_REPEAT_TOGGLE) ? "toggle"
                               : std::to_string(ab[2]);
            report.unnamed_buttons[name] = "<macro, repeat=" + repeat +
                                           "; the events cannot be read back yet>";
            report.warnings.push_back(
                name + " runs a macro (repeat=" + repeat + "); its events are "
                "not read back, so they are missing from this file");
            continue;
        }

        std::string action = action_name(ab);
        if (action.empty()) {
            report.unnamed_buttons[name] = "<" + hex2(ab[0]) + " " + hex2(ab[1]) +
                                           " " + hex2(ab[2]) + " " + hex2(ab[3]) + ">";
            report.warnings.push_back(name + " holds action bytes with no known name");
            continue;
        }
        out.buttons[name] = action;
        ++decoded_fields;
    }

    return decoded_fields > 0;
}

// -----------------------------------------------------------------------
// Which blocks to ask for
// -----------------------------------------------------------------------

uint16_t request_address(const Packet& req) {
    return static_cast<uint16_t>((req[3] << 8) | req[4]);
}

std::vector<const Packet*> config_read_requests(uint16_t addr_limit) {
    std::vector<const Packet*> reads;
    for (const Packet& p : M913_READ_CODES) {
        if (p[1] != 0x08 || p[5] != 0x0a) continue;
        if (request_address(p) >= addr_limit) continue;
        reads.push_back(&p);
    }
    return reads;
}
