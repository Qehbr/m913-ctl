#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"
#include "data.h"
#include "protocol.h"
#include "readback.h"
#include "usb.h"

static volatile sig_atomic_t g_stop = 0;
static void handle_stop(int) { g_stop = 1; }

// Install the stop handler for both SIGINT and SIGTERM.
//
// SIGTERM matters as much as SIGINT here: its default action terminates the
// process outright, so UsbMouse::close() never runs and the kernel HID driver
// is left detached — the mouse stops working entirely until it is replugged.
// Anything that kills the tool without a terminal (a script, a service
// manager, a wrapper calling Popen.terminate()) sends SIGTERM, not SIGINT.
//
// The handler only raises a flag. The listen loops poll it and exit cleanly;
// a config write in progress finishes its packet sequence first, which is
// deliberate — a half-applied config is worse than a second's delay.
static void install_stop_handlers() {
    std::signal(SIGINT,  handle_stop);
    std::signal(SIGTERM, handle_stop);
}

// -----------------------------------------------------------------------
// Version
// -----------------------------------------------------------------------
static constexpr const char* VERSION = APP_VERSION;

// -----------------------------------------------------------------------
// Help text
// -----------------------------------------------------------------------
static void print_help(const char* prog) {
    std::cout <<
R"(Usage: )" << prog << R"( [OPTIONS]

Redragon M913 Impact Elite configuration tool for Linux.

Options:
  -h, --help               Show this help and exit
  -V, --version            Show version and exit

  --listen [EP]            Passively listen for packets from the mouse.
                           EP 0x81 = mouse HID (7B), EP 0x82 = config (17B)
                           Default: listens on both. Ctrl+C to stop.
                           Press mouse buttons to see raw packets.

  --probe                  Show USB interfaces and endpoints for the device

  -c, --config FILE        Apply settings from an INI config file
  --save [FILE]            Read the current configuration off the mouse and
                           write it as an INI file (stdout if FILE is omitted).
                           The output is what --config accepts back, so this
                           is the way to get an editable copy of a setup you
                           made with the vendor software. Areson only; keep
                           the mouse moving while it runs.

  --dpi SLOT=VALUE         Set a DPI slot (1-5), e.g. --dpi 2=3200
  --led MODE               Set LED mode: off, rainbow, steady, respiration
  --led-color RRGGBB       LED colour for steady/respiration, e.g. ff0000
  --led-brightness N       LED brightness 0-255 (steady/respiration/rainbow)
  --led-speed N            Respiration/rainbow speed 1-5 (1=slowest)
  --polling-rate HZ        Set USB polling rate: 125, 250, 500, or 1000 (Hz)
  --button NAME=ACTION     Remap a button, e.g. --button side1=f1
                           NAME: side1..12, left, right, middle, fire
                           (run --list-actions for valid action names)

  --list-actions           Print all valid button action names and exit

  --probe-commands         Probe which command bytes the device responds to
                           (reverse-engineering aid)

  --get [N]                Raw form of --save, for protocol work: print the
                           replies as hex instead of decoding them. N = one
                           block (0-68); omit N to walk all of them,
                           including the 16 regions at 0x0301+ that --save
                           skips. Codes are Areson-derived.

  --raw-send HEX           Send a raw packet and stay in listen mode.
                           HEX = space-separated bytes (up to 16).
                           Bytes are zero-padded to 16; checksum is
                           appended as byte 16 automatically.
                           e.g. --raw-send "08 07 00 00 60 08"

Examples:
  m913-ctl --probe
  m913-ctl --listen
  m913-ctl --config examples/example.ini
  m913-ctl --save my-setup.ini        # read the mouse's current config
  m913-ctl --led rainbow
  m913-ctl --led steady --led-color ff0000 --led-brightness 200
  m913-ctl --dpi 1=800 --dpi 2=1600 --dpi 3=3200 --dpi 4=6400 --dpi 5=7200
  m913-ctl --button side1=f1 --button side2=f2
  m913-ctl --button fire="fire:50:2"     # fire button: speed=50, repeat=2 times
  m913-ctl --button side3=media_play --button side4=media_vol_up
  m913-ctl --button side5="ctrl+c" --button side6="a+b"  # key combinations

Note: --button and --dpi write a COMPLETE block each. Any button or DPI slot
you do not mention is reset to its factory default — the mouse has no way to
change one entry in isolation. Pass everything you want in a single command,
or keep it in a config file; --save writes one out for you to edit.

Note: Run as root or install the udev rule for non-root access:
  sudo cp udev/99-m913.rules /usr/lib/udev/rules.d/
  sudo udevadm control --reload-rules && sudo udevadm trigger
)";
}

// -----------------------------------------------------------------------
// Send one packet and read the ACK interrupt response.
// The device always sends a 17-byte ACK on EP 0x82 after each config write.
// We wait up to 1000 ms — if it times out we warn and continue
// (wireless latency can be high).
// -----------------------------------------------------------------------
static void send_cmd(UsbMouse& mouse, const Packet& p, const std::string& label) {
    if (!label.empty())
        std::cout << "  " << label << "\n";
    std::cout << "    --> ";
    hexdump_packet(p);
    mouse.send(p.data());

    // Poll for the 17-byte ACK on EP 0x82.
    // The mouse responds within ~20 ms on native USB.  On WSL2/USB-IP the
    // VHCI may need a fresh URB already queued to catch interrupt data, so
    // submit 15 × 100 ms reads (1.5 s total) instead of one big wait.
    uint8_t buf[M913_PACKET_SIZE] = {};
    int got = 0;
    for (int attempt = 0; attempt < 15 && got == 0; ++attempt)
        got = mouse.try_recv(buf, M913_PACKET_SIZE, INTERRUPT_EP_IN, 100);

    if (got > 0) {
        std::cout << "    <-- ";
        std::cout << std::hex << std::setfill('0');
        for (int b = 0; b < got; ++b)
            std::cout << std::setw(2) << static_cast<int>(buf[b]) << " ";
        std::cout << std::dec << "\n";
    } else {
        std::cout << "    <-- (no ACK within 1.5s)\n";
    }
}

// Send an entire packet sequence (keyboard-key sub-packets + config packets).
static void send_sequence(UsbMouse& mouse,
                          const std::vector<Packet>& pkts,
                          const std::string& heading) {
    if (pkts.empty()) return;
    std::cout << "=== " << heading << " (" << pkts.size() << " packets) ===\n";
    for (size_t i = 0; i < pkts.size(); ++i)
        send_cmd(mouse, pkts[i], "pkt " + std::to_string(i + 1) + "/" +
                                  std::to_string(pkts.size()));
}

// -----------------------------------------------------------------------
// Apply a full config to the mouse
//
// What to send is decided by build_config_sequences() in config.cpp, which
// touches no hardware — this is only the I/O half.
// -----------------------------------------------------------------------
static void apply_config(UsbMouse& mouse, const Config& cfg,
                         const uint8_t* btn_layout = nullptr,
                         bool is_compx = false) {
    for (auto& seq : build_config_sequences(cfg, btn_layout, is_compx))
        send_sequence(mouse, seq.packets, seq.label);
}

// -----------------------------------------------------------------------
// Send one read request and wait for the reply that belongs to it.
//
// EP 0x82 carries HID input reports as well as config replies, and a wired
// mouse in active use delivers a lot of them. Taking the first packet that
// turns up therefore does double damage: the real reply is lost, and the next
// request picks it up instead — which is how a sweep ends up printing a
// payload under an address nobody asked for. So match on report ID 0x09 plus
// the request's own address echoed back in bytes [3..4].
//
// 15 × 100 ms is the same budget send_cmd() gives an ACK: replies come from
// the mouse rather than the receiver, so an idle wireless mouse needs the
// full 1.5 s.
//
// Returns the number of bytes received, 0 if no matching reply arrived.
// `skipped` is set to how many unrelated packets were discarded.
// -----------------------------------------------------------------------
static int fetch_block(UsbMouse& mouse, const Packet& req,
                       uint8_t rx[M913_PACKET_SIZE], int& skipped) {
    skipped = 0;
    mouse.send(req.data());

    for (int attempt = 0; attempt < 15; ++attempt) {
        int got = mouse.try_recv(rx, M913_PACKET_SIZE, INTERRUPT_EP_IN, 100);
        if (got <= 0) continue;
        if (rx[0] == 0x09 && rx[3] == req[3] && rx[4] == req[4]) return got;
        ++skipped;
    }
    return 0;
}

// -----------------------------------------------------------------------
// Read every block a decode needs, assembling the device's memory image.
//
// Each block gets a second attempt if the first brings back nothing, which
// is what an idle wireless mouse does. Progress goes through std::cout, which
// the caller has pointed at stderr when the INI is bound for stdout.
//
// Returns the number of blocks that answered.
// -----------------------------------------------------------------------
static size_t read_config_image(UsbMouse& mouse, BlockMap& out,
                               DecodeReport& report) {
    std::vector<const Packet*> reads = config_read_requests();

    std::cout << "Reading " << reads.size() << " blocks"
              << " (keep the mouse moving — the replies come from the mouse"
                 " itself, not the receiver)\n";

    size_t answered = 0, bad_checksum = 0;
    std::vector<const Packet*> todo = reads;

    for (int pass = 0; pass < 2 && !todo.empty(); ++pass) {
        if (pass == 1)
            std::cout << "Retrying " << todo.size()
                      << " block(s) that did not answer\n";

        std::vector<const Packet*> failed;
        for (const Packet* req : todo) {
            if (g_stop) break;
            uint8_t rx[M913_PACKET_SIZE] = {};
            int     skipped = 0;
            if (fetch_block(mouse, *req, rx, skipped) < M913_PACKET_SIZE) {
                failed.push_back(req);
                continue;
            }
            if (!verify_reply_checksum(rx)) {
                ++bad_checksum;
                std::ostringstream w;
                w << "reply for 0x" << std::hex << std::setw(4) << std::setfill('0')
                  << request_address(*req) << " failed its checksum";
                report.warnings.push_back(w.str());
            }
            std::array<uint8_t, READ_CHUNK> chunk{};
            for (size_t i = 0; i < READ_CHUNK; ++i) chunk[i] = rx[6 + i];
            out[request_address(*req)] = chunk;
            ++answered;
        }
        todo = failed;
    }

    for (const Packet* req : todo)
        report.missing.push_back(request_address(*req));

    std::cout << "Read " << answered << " of " << reads.size() << " blocks";
    if (bad_checksum) std::cout << " (" << bad_checksum << " with a bad checksum)";
    std::cout << "\n";
    return answered;
}

// -----------------------------------------------------------------------
// Print one configuration block as raw hex (--get)
//
// The decoding lives in readback.cpp and is reached through --save; this is
// the unfiltered view, kept for protocol work: it shows the reply exactly as
// it arrived, including for the addresses --save has nothing to say about.
//
// The index is the caller's responsibility to bound; it is validated during
// option parsing, before the device is opened.
// -----------------------------------------------------------------------

static void get_block(UsbMouse& mouse, size_t index) {
    const Packet& req = M913_READ_CODES[index];

    std::cout << "  [" << std::setw(2) << std::setfill(' ') << index << "] --> ";
    hexdump_packet(req);

    uint8_t buf[M913_PACKET_SIZE] = {};
    int     skipped = 0;
    int     got     = fetch_block(mouse, req, buf, skipped);
    bool    matched = got > 0;

    std::cout << "       <-- ";
    if (!matched) {
        std::cout << "(no matching reply";
        if (skipped)
            std::cout << "; discarded " << skipped << " unrelated packet(s)";
        std::cout << " — if wireless, move the mouse)\n";
        return;
    }
    std::cout << std::hex << std::setfill('0');
    for (int b = 0; b < got; ++b)
        std::cout << std::setw(2) << static_cast<int>(buf[b]) << " ";
    std::cout << std::dec << std::setfill(' ');
    if (got != M913_PACKET_SIZE)
        std::cout << " (short reply: " << got << " of " << M913_PACKET_SIZE << " bytes)";
    if (got == M913_PACKET_SIZE && !verify_reply_checksum(buf))
        std::cout << " (bad checksum)";
    if (skipped)
        std::cout << " (skipped " << skipped << " input report(s))";
    std::cout << "\n";
}

// -----------------------------------------------------------------------
// Read the configuration off the mouse and write it out as INI (--save)
//
// `out` is the stream the INI itself goes to; everything else this prints is
// status and goes through std::cout, which the caller has already pointed at
// stderr when the INI is bound for stdout.
//
// Returns false if nothing usable came back.
// -----------------------------------------------------------------------
static bool save_config(UsbMouse& mouse, std::ostream& out,
                        const std::string& device_id) {
    BlockMap     image;
    DecodeReport report;
    read_config_image(mouse, image, report);

    Config cfg;
    if (!decode_device_config(image, cfg, report)) {
        std::cerr << "Error: could not decode a configuration from the mouse";
        if (image.empty())
            std::cerr << " — no block answered. If this is the wireless"
                         " receiver, keep the mouse moving and try again";
        std::cerr << "\n";
        return false;
    }

    std::string header =
        "Written by m913-ctl " + std::string(VERSION) + " --save, read from " +
        device_id + "\n"
        "\n"
        "Applying this file rewrites ALL 16 buttons and ALL 5 DPI slots: the\n"
        "mouse stores each as one block, so there is no way to change part of\n"
        "one. That is fine here — this file describes the whole state.\n";

    if (!report.missing.empty()) {
        header += "\nINCOMPLETE: " + std::to_string(report.missing.size()) +
                  " block(s) never answered, so some settings below are\n"
                  "absent rather than wrong. Re-run with the mouse moving.\n";
    }
    if (!report.warnings.empty()) {
        header += "\nNotes from the decoder:\n";
        for (auto& w : report.warnings) header += "  - " + w + "\n";
    }

    out << config_to_ini(cfg, header, report.unnamed_buttons);
    out.flush();

    for (auto& w : report.warnings)
        std::cerr << "Warning: " << w << "\n";
    if (!report.missing.empty()) {
        std::cerr << "Warning: " << report.missing.size()
                  << " block(s) did not answer; the output is incomplete.\n";
    }
    return true;
}

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help(argv[0]);
        return 0;
    }

    // ---- option definitions ----
    struct option long_opts[] = {
        {"help",            no_argument,       nullptr, 'h'},
        {"version",         no_argument,       nullptr, 'V'},
        {"listen",          optional_argument, nullptr, 1006},
        {"probe",           no_argument,       nullptr, 1007},
        {"probe-commands",  no_argument,       nullptr, 1008},
        {"raw-send",        required_argument, nullptr, 1009},
        {"config",          required_argument, nullptr, 'c'},
        {"dpi",             required_argument, nullptr, 1001},
        {"led",             required_argument, nullptr, 1002},
        {"button",          required_argument, nullptr, 1003},
        {"list-actions",    no_argument,       nullptr, 1004},
        {"polling-rate",    required_argument, nullptr, 1011},
        {"get",             optional_argument, nullptr, 1012},
        {"save",            optional_argument, nullptr, 1013},
        {"led-color",       required_argument, nullptr, 1014},
        {"led-brightness",  required_argument, nullptr, 1015},
        {"led-speed",       required_argument, nullptr, 1016},
        {nullptr, 0, nullptr, 0}
    };

    // ---- collect requested operations ----
    bool        do_probe          = false;
    bool        do_listen         = false;
    bool        do_probe_commands = false;
    bool        do_get            = false;
    bool        do_save           = false;
    int         get_index    = -1;  // -1 = every block
    int         listen_ep    = -1;  // -1 = auto (try 0x81 and 0x82)
    std::string config_file;
    std::string save_file;         // empty with do_save = write to stdout
    std::string raw_send_hex;

    struct DpiArg  { int slot; uint16_t value; };
    struct BtnArg  { std::string name; std::string action; };

    std::vector<DpiArg>   dpi_args;
    std::vector<BtnArg>   btn_args;
    uint16_t              polling_rate_arg = 0;  // 0 = not set

    // Inline LED arguments. Each is only applied when its flag was given, so
    // --led-color on its own can recolour without disturbing the mode the
    // config file (or the device) already has.
    bool     led_mode_set  = false;
    LedMode  led_mode_arg  = LedMode::Rainbow;
    bool     led_color_set = false;
    uint32_t led_color_arg = 0;
    bool     led_bright_set = false;
    uint8_t  led_bright_arg = 0;
    bool     led_speed_set  = false;
    uint8_t  led_speed_arg  = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "hVc:", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'h':
            print_help(argv[0]);
            return 0;

        case 'V':
            std::cout << "m913-ctl " << VERSION << "\n";
            return 0;

        case 'c':
            config_file = optarg;
            break;

        case 1001: {  // --dpi SLOT=VALUE
            std::string arg = optarg;
            auto eq = arg.find('=');
            if (eq == std::string::npos) {
                std::cerr << "Error: --dpi expects SLOT=VALUE (e.g. --dpi 2=3200)\n";
                return 1;
            }
            try {
                int slot = std::stoi(arg.substr(0, eq));
                int val  = std::stoi(arg.substr(eq + 1));
                if (slot < 1 || slot > DPI_SLOTS) {
                    std::cerr << "Error: DPI slot must be 1-" << DPI_SLOTS << "\n";
                    return 1;
                }
                if (val <= 0 || val > 65535) {
                    std::cerr << "Error: DPI value out of range\n";
                    return 1;
                }
                // Which values are actually supported depends on the hardware
                // revision, which is not known until the device is opened —
                // checked below, before anything is sent.
                dpi_args.push_back({slot, static_cast<uint16_t>(val)});
            } catch (...) {
                std::cerr << "Error: invalid --dpi argument: " << arg << "\n";
                return 1;
            }
            break;
        }

        case 1002: {  // --led MODE
            // Resolved here rather than after the device is opened, so an
            // unknown mode fails before anything is claimed or written.
            std::string sl = optarg;
            for (auto& c : sl)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if      (sl == "off")         led_mode_arg = LedMode::Off;
            else if (sl == "rainbow")     led_mode_arg = LedMode::Rainbow;
            else if (sl == "static")      led_mode_arg = LedMode::Steady;
            else if (sl == "steady")      led_mode_arg = LedMode::Steady;
            else if (sl == "breathing")   led_mode_arg = LedMode::Respiration;
            else if (sl == "respiration") led_mode_arg = LedMode::Respiration;
            else {
                std::cerr << "Error: unknown LED mode '" << optarg
                          << "'. Valid: off, rainbow, steady, respiration\n";
                return 1;
            }
            led_mode_set = true;
            break;
        }

        case 1003: {  // --button NAME=ACTION
            std::string arg = optarg;
            auto eq = arg.find('=');
            if (eq == std::string::npos) {
                std::cerr << "Error: --button expects NAME=ACTION "
                          << "(e.g. --button side1=f1)\n";
                return 1;
            }
            // Accept both "side1=..." and "left=..." (add "button_" prefix)
            std::string bname = arg.substr(0, eq);
            if (bname.rfind("button_", 0) != 0)
                bname = "button_" + bname;
            btn_args.push_back({bname, arg.substr(eq + 1)});
            break;
        }

        case 1004:  // --list-actions
            list_actions();
            return 0;

        case 1006: {  // --listen [EP]
            do_listen = true;
            // getopt only fills optarg for an optional argument when it is
            // attached as --listen=0x82. The help text shows the separated
            // form, and that used to be accepted and then silently ignored,
            // so take the next argv too when it looks like an endpoint.
            const char* ep = optarg;
            if (!ep && optind < argc && argv[optind][0] != '-')
                ep = argv[optind++];
            if (ep) {
                try {
                    listen_ep = std::stoi(ep, nullptr, 16);
                } catch (...) {
                    std::cerr << "Error: invalid endpoint '" << ep
                              << "' (expect hex, e.g. 0x81)\n";
                    return 1;
                }
            }
            break;
        }

        case 1007:  // --probe
            do_probe = true;
            break;

        case 1008:  // --probe-commands
            do_probe_commands = true;
            break;

        case 1009:  // --raw-send HEX
            raw_send_hex = optarg;
            break;

        case 1011: {  // --polling-rate HZ
            try {
                int r = std::stoi(optarg);
                if (r != 125 && r != 250 && r != 500 && r != 1000) {
                    std::cerr << "Error: --polling-rate must be 125, 250, 500, or 1000\n";
                    return 1;
                }
                polling_rate_arg = static_cast<uint16_t>(r);
            } catch (...) {
                std::cerr << "Error: invalid --polling-rate argument\n";
                return 1;
            }
            break;
        }

        case 1012: {  // --get [N]
            do_get = true;
            // Same two-form handling as --listen: getopt only fills optarg for
            // an optional argument when it is attached as --get=5, but the help
            // text shows the separated form, so take the next argv too when it
            // does not look like another option.
            const char* idx = optarg;
            if (!idx && optind < argc && argv[optind][0] != '-')
                idx = argv[optind++];
            if (idx) {
                try {
                    size_t consumed = 0;
                    int    n        = std::stoi(idx, &consumed);
                    if (consumed != std::string(idx).size())
                        throw std::invalid_argument("trailing characters");
                    // Bounded here, before the device is opened: get_block()
                    // indexes M913_READ_CODES directly, so an out-of-range
                    // value would read past the table and transmit whatever
                    // followed it as a config packet.
                    if (n < 0 || static_cast<size_t>(n) >= M913_READ_CODE_COUNT) {
                        std::cerr << "Error: --get index must be 0-"
                                  << M913_READ_CODE_COUNT - 1 << " (got " << n << ")\n";
                        return 1;
                    }
                    get_index = n;
                } catch (...) {
                    std::cerr << "Error: invalid --get index '" << idx
                              << "' (expect 0-" << M913_READ_CODE_COUNT - 1 << ")\n";
                    return 1;
                }
            }
            break;
        }

        case 1013: {  // --save [FILE]
            do_save = true;
            // Same two-form handling as --listen and --get.
            const char* f = optarg;
            if (!f && optind < argc && argv[optind][0] != '-')
                f = argv[optind++];
            if (f) save_file = f;
            break;
        }

        case 1014: {  // --led-color RRGGBB
            std::string hex = optarg;
            if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
            try {
                size_t consumed = 0;
                unsigned long v = std::stoul(hex, &consumed, 16);
                if (hex.size() != 6 || consumed != 6) throw std::invalid_argument("");
                led_color_arg = static_cast<uint32_t>(v);
                led_color_set = true;
            } catch (...) {
                std::cerr << "Error: --led-color expects 6 hex digits (e.g. ff0000), got '"
                          << optarg << "'\n";
                return 1;
            }
            break;
        }

        case 1015: {  // --led-brightness N
            try {
                int v = std::stoi(optarg);
                if (v < 0 || v > 255) throw std::out_of_range("");
                led_bright_arg = static_cast<uint8_t>(v);
                led_bright_set = true;
            } catch (...) {
                std::cerr << "Error: --led-brightness must be 0-255\n";
                return 1;
            }
            break;
        }

        case 1016: {  // --led-speed N
            try {
                int v = std::stoi(optarg);
                if (v < 1 || v > 5) throw std::out_of_range("");
                led_speed_arg = static_cast<uint8_t>(v);
                led_speed_set = true;
            } catch (...) {
                std::cerr << "Error: --led-speed must be 1-5 (1=slowest)\n";
                return 1;
            }
            break;
        }

        default:
            std::cerr << "Use --help for usage.\n";
            return 1;
        }
    }

    // ---- validate --button arguments up front ----
    // Done before the device is opened, because --dpi and --led are applied
    // ahead of buttons: a binding rejected mid-run would leave those already
    // written and skip the commit packet, i.e. a half-applied config.
    for (auto& [name, action_str] : btn_args) {
        Button btn;
        if (!parse_button_name(name, btn)) {
            std::cerr << "Error: unknown button name '" << name << "'\n";
            return 1;
        }
        ActionBytes ab;
        if (!parse_action(action_str, ab)) {
            std::cerr << "Error: unknown action '" << action_str << "'\n";
            return 1;
        }
        size_t tokens = action_combo_tokens(ab);
        if (tokens > MAX_COMBO_TOKENS) {
            std::cerr << "Error: action '" << action_str << "' for " << name
                      << " combines " << tokens << " modifiers+keys — the mouse "
                      << "stores at most " << MAX_COMBO_TOKENS << " per binding\n";
            return 1;
        }
    }

    // ---- validate that there's something to do ----
    bool has_led_arg = led_mode_set || led_color_set || led_bright_set || led_speed_set;
    bool has_work = do_probe || do_probe_commands || do_listen || do_get || do_save ||
                    !raw_send_hex.empty() ||
                    !config_file.empty() ||
                    !dpi_args.empty() || has_led_arg || !btn_args.empty() ||
                    polling_rate_arg != 0;
    if (!has_work) {
        print_help(argv[0]);
        return 0;
    }

    // `--save` with no FILE puts the INI on stdout, which has to be the only
    // thing there — otherwise `m913-ctl --save > my.ini` produces a file that
    // starts with connection chatter and packet dumps. Rather than thread a
    // stream parameter through every print below, point std::cout at stderr
    // for the rest of the run and keep the real stdout for the INI.
    std::streambuf* real_stdout = std::cout.rdbuf();
    if (do_save && save_file.empty())
        std::cout.rdbuf(std::cerr.rdbuf());

    // Install before the device is opened, so every path that can claim a USB
    // interface is covered by the cleanup on the way out.
    install_stop_handlers();

    // ---- open mouse ----
    UsbMouse mouse;
    const uint8_t* btn_layout = nullptr;
    bool           is_compx   = false;
    uint16_t       dev_vid    = 0;
    uint16_t       dev_pid    = 0;
    try {
        uint16_t vid = M913_VID, pid = M913_PID;
        // Wired PIDs come FIRST, deliberately.
        //
        // The wired PID is the mouse itself; the wireless PID is only the
        // receiver. Plugging the cable in makes the mouse switch to wired and
        // stop listening on 2.4G, but it does NOT make the dongle disappear —
        // so with both connected, probing the receiver first opens a device
        // that enumerates fine and then answers nothing, because the packets
        // are being radioed at a mouse that is no longer on the radio.
        //
        // Preferring the direct connection is strictly more reliable: reaching
        // the mouse over its own USB cable never depends on a live radio link,
        // and when no cable is present these entries simply fail to open and
        // the receiver is used as before.
        const std::vector<std::pair<uint16_t,uint16_t>> candidates = {
            {M913_VID,  M913_PID_WIRED},
            {M913_VID,  M913_PID},
            {COMPX_VID, COMPX_PID_WIRED},
            {COMPX_VID, COMPX_PID},
        };
        bool opened = false;
        for (auto [v, p] : candidates) {
            try {
                mouse.open_all_interfaces(v, p);
                vid = v; pid = p; opened = true;
                break;
            } catch (...) {}
        }
        if (!opened)
            throw std::runtime_error("Could not find any supported M913 variant — is the mouse plugged in? Try running with sudo or install the udev rule.");

        dev_vid    = vid;
        dev_pid    = pid;
        is_compx   = (vid == COMPX_VID);
        btn_layout = is_compx ? COMPX_LAYOUT : nullptr;
        if (is_compx)
            mouse.set_ctrl_value(0x0208);  // Compx uses output report, not feature report

        std::cout << "Connected (" << std::hex
                  << std::setw(4) << std::setfill('0') << vid << ":"
                  << std::setw(4) << std::setfill('0') << pid
                  << std::dec << ").\n";

        // Drain any spontaneous init/hello packet from the wireless device.
        uint8_t init_buf[64] = {};
        int init_got = mouse.try_recv(init_buf, sizeof(init_buf), INTERRUPT_EP_IN, 800);
        if (init_got > 0) {
            std::cout << "[init packet (" << init_got << "B)]: ";
            std::cout << std::hex << std::setfill('0');
            for (int b = 0; b < init_got; ++b)
                std::cout << std::setw(2) << static_cast<int>(init_buf[b]) << " ";
            std::cout << std::dec << "\n";
        }
        std::cout << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    int exit_code = 0;

    try {
        // ---- build the configuration to write ----
        //
        // The file and the inline flags are merged into ONE Config and applied
        // once. They used to be applied one after the other, which meant
        // `--config f.ini --button side1=x` sent two complete button-mapping
        // sequences and the second — carrying only side1 — reset every button
        // the file had just set. Overlaying instead makes the inline flags
        // behave like overrides, which is what the syntax suggests.
        //
        // Parsed and validated here, before any diagnostic mode runs, so a
        // typo in the file or an unsupported DPI value cannot be discovered
        // after --listen has already blocked for a minute.
        Config cfg;
        bool   do_write = false;

        if (!config_file.empty()) {
            std::cout << "=== Reading config: " << config_file << " ===\n";
            cfg      = parse_config_file(config_file);
            do_write = true;
        }

        for (auto& [slot, val] : dpi_args) {
            if (slot >= 1 && slot <= DPI_SLOTS) {
                cfg.dpi[slot - 1].value = val;
                do_write = true;
            }
        }

        if (led_mode_set)  { cfg.led.mode = led_mode_arg;  cfg.led.set = true; do_write = true; }
        if (led_color_set) { cfg.led.color = led_color_arg; cfg.led.set = true; do_write = true; }
        if (led_bright_set){ cfg.led.brightness = led_bright_arg; cfg.led.set = true; do_write = true; }
        if (led_speed_set) { cfg.led.speed = led_speed_arg; cfg.led.set = true; do_write = true; }

        if (polling_rate_arg != 0) {
            cfg.mouse.polling_rate = polling_rate_arg;
            cfg.mouse.set          = true;
            do_write               = true;
        }

        for (auto& [name, action_str] : btn_args) {
            cfg.buttons[name] = action_str;
            do_write          = true;
        }

        if (do_write)
            validate_config(cfg, is_compx);

        // ---- --probe ----
        if (do_probe) {
            std::cout << "=== USB endpoint probe ===\n";
            mouse.probe();
        }

        // ---- --probe-commands ----
        if (do_probe_commands) {
            std::cout << "=== Probing command bytes (0x01..0x20) ===\n";
            std::cout << "Sending feature report #8 with varying byte 0...\n\n";

            uint8_t buf[17] = {};
            for (int cmd = 0x01; cmd <= 0x20; ++cmd) {
                Packet pkt{};
                pkt[0] = static_cast<uint8_t>(cmd);
                pkt[16] = compute_checksum(pkt);

                std::cout << "cmd=0x" << std::hex << std::setw(2) << std::setfill('0')
                          << cmd << std::dec << "  ";
                std::cout.flush();

                try {
                    mouse.send(pkt.data());
                } catch (const std::exception& e) {
                    std::cout << "SEND ERROR: " << e.what() << "\n";
                    continue;
                }

                int got = mouse.try_recv(buf, sizeof(buf), INTERRUPT_EP_IN, 300);
                if (got > 0) {
                    std::cout << "RESPONSE (" << got << "B): ";
                    std::cout << std::hex << std::setfill('0');
                    for (int b = 0; b < got; ++b)
                        std::cout << std::setw(2) << static_cast<int>(buf[b]) << " ";
                    std::cout << std::dec << "  *** HIT ***\n";
                } else {
                    std::cout << "no response\n";
                }
            }
            std::cout << "\nDone.\n";
        }

        // ---- --get [N] ----
        // Grouped with the other read-only diagnostics, and ahead of --listen
        // and --raw-send: both of those block until Ctrl+C, so anything after
        // them never runs.
        if (do_get) {
            if (is_compx)
                std::cerr << "Warning: --get codes were captured from Areson "
                             "hardware; this device is Compx, which uses "
                             "different addressing. Replies may be meaningless.\n";

            if (get_index >= 0) {
                std::cout << "=== Reading config block " << get_index << " ===\n";
                get_block(mouse, static_cast<size_t>(get_index));
            } else {
                std::cout << "=== Reading all " << M913_READ_CODE_COUNT
                          << " config blocks ===\n";
                for (size_t i = 0; i < M913_READ_CODES.size(); ++i)
                    get_block(mouse, i);
            }
            std::cout << "\nDone. (raw replies — use --save for a decoded config)\n";
        }

        // ---- --save [FILE] ----
        // Placed with the other read-only modes and ahead of any write, so a
        // run that both saves and applies captures the configuration as it
        // was found rather than the one it is about to install.
        if (do_save) {
            if (is_compx) {
                std::cerr << "Error: --save decodes the Areson layout only. The Compx"
                             " revision answers a different report type at addresses"
                             " that have never been captured, so there is nothing"
                             " reliable to decode. Use --get to see the raw replies.\n";
                exit_code = 1;
                goto cleanup;
            }

            std::ostringstream id;
            id << std::hex << std::setw(4) << std::setfill('0') << dev_vid << ":"
               << std::setw(4) << std::setfill('0') << dev_pid;

            if (save_file.empty()) {
                std::ostream ini(real_stdout);
                if (!save_config(mouse, ini, id.str())) {
                    exit_code = 1;
                    goto cleanup;
                }
            } else {
                std::ofstream f(save_file);
                if (!f) {
                    std::cerr << "Error: cannot write " << save_file << "\n";
                    exit_code = 1;
                    goto cleanup;
                }
                if (!save_config(mouse, f, id.str())) {
                    exit_code = 1;
                    goto cleanup;
                }
                f.close();
                std::cout << "Wrote " << save_file << "\n";
            }
        }

        // ---- --raw-send HEX ----
        if (!raw_send_hex.empty()) {
            std::cout << "=== Raw send ===\n";

            Packet pkt{};
            std::istringstream iss(raw_send_hex);
            std::string token;
            int byte_idx = 0;
            while (iss >> token && byte_idx < M913_PACKET_SIZE - 1) {
                try {
                    pkt[byte_idx++] = static_cast<uint8_t>(std::stoul(token, nullptr, 16));
                } catch (...) {
                    std::cerr << "Error: invalid hex byte '" << token << "' in --raw-send\n";
                    exit_code = 1;
                    goto cleanup;
                }
            }
            pkt[M913_PACKET_SIZE - 1] = compute_checksum(pkt);

            std::cout << "Sending: ";
            hexdump_packet(pkt);
            mouse.send(pkt.data());

            {
                uint8_t buf[64] = {};
                for (uint8_t ep : {INTERRUPT_EP_IN, static_cast<uint8_t>(0x81)}) {
                    int got = mouse.try_recv(buf, sizeof(buf), ep, 500);
                    if (got > 0) {
                        std::cout << "Response EP 0x" << std::hex << std::setw(2)
                                  << std::setfill('0') << static_cast<int>(ep)
                                  << " (" << std::dec << got << "B): ";
                        std::cout << std::hex << std::setfill('0');
                        for (int b = 0; b < got; ++b)
                            std::cout << std::setw(2) << static_cast<int>(buf[b]) << " ";
                        std::cout << std::dec << "\n";
                    }
                }
            }

            // Stay in listen mode so the user can verify the effect without
            // needing a second terminal.
            std::cout << "\nPacket sent. Press buttons to verify effect. Ctrl+C to stop.\n\n";
            struct EpInfo { uint8_t addr; int maxpkt; };
            const EpInfo verify_eps[] = { {0x81, 7}, {0x82, 17} };
            int vpkt = 0;
            uint8_t vbuf[64] = {};
            while (!g_stop) {
                for (auto& ep_info : verify_eps) {
                    int got = mouse.try_recv(vbuf, sizeof(vbuf), ep_info.addr, 200);
                    if (got > 0) {
                        std::cout << "[pkt " << ++vpkt << " | EP 0x"
                                  << std::hex << std::setw(2) << std::setfill('0')
                                  << static_cast<int>(ep_info.addr) << " | "
                                  << std::dec << got << "B]  ";
                        std::cout << std::hex << std::setfill('0');
                        for (int b = 0; b < got; ++b)
                            std::cout << std::setw(2) << static_cast<int>(vbuf[b]) << " ";
                        std::cout << std::dec << "\n";
                        std::cout.flush();
                    }
                    if (g_stop) break;
                }
            }
            std::cout << "Stopped.\n";
        }

        // ---- --listen ----
        if (do_listen) {

            struct EpInfo { uint8_t addr; int maxpkt; };
            const EpInfo known_eps[] = { {0x81, 7}, {0x82, 17} };
            const int n_eps = (listen_ep >= 0) ? 1 : 2;

            uint8_t buf[64] = {};

            std::cout << "=== Listening for packets (Ctrl+C to stop) ===\n";
            if (listen_ep >= 0)
                std::cout << "Endpoint: 0x" << std::hex << listen_ep << std::dec << "\n";
            else
                std::cout << "Endpoints: 0x81 (mouse, 7B)  0x82 (config, 17B)\n";
            std::cout << "Press mouse buttons now...\n\n";

            int pkt_count = 0;
            while (!g_stop) {
                for (int i = 0; i < n_eps && !g_stop; ++i) {
                    uint8_t ep  = (listen_ep >= 0)
                                    ? static_cast<uint8_t>(listen_ep)
                                    : known_eps[i].addr;
                    int bufsz   = (listen_ep >= 0) ? 64 : known_eps[i].maxpkt;
                    int got = mouse.try_recv(buf, bufsz, ep, 200);
                    if (got > 0) {
                        std::cout << "[pkt " << ++pkt_count << " | EP 0x"
                                  << std::hex << std::setw(2) << std::setfill('0')
                                  << static_cast<int>(ep) << " | " << std::dec
                                  << got << "B]  ";
                        std::cout << std::hex << std::setfill('0');
                        for (int b = 0; b < got; ++b)
                            std::cout << std::setw(2) << static_cast<int>(buf[b]) << " ";
                        std::cout << std::dec << "\n";
                        std::cout.flush();
                    }
                }
            }
            std::cout << "\nStopped.\n";
        }

        // ---- apply the merged configuration ----
        // One Config, one pass, whether it came from a file, from inline
        // flags, or from both. Built and validated at the top of this block.
        if (do_write) {
            std::cout << "=== Applying configuration ===\n";
            apply_config(mouse, cfg, btn_layout, is_compx);
        }

        // ---- commit ----
        // The Redragon software always ends a config session with two
        // "08 04 00..." packets (observed in USB captures).  These appear
        // to act as a commit/apply-to-flash command.
        if (do_write) {
            Packet commit{};
            commit[0] = 0x08;
            commit[1] = 0x04;
            commit[16] = compute_checksum(commit);  // = 0x49
            send_sequence(mouse, {commit, commit}, "Commit");
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        exit_code = 1;
    }

cleanup:
    mouse.close();
    return exit_code;
}
