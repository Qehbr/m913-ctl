#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <atomic>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "config.h"
#include "data.h"
#include "protocol.h"
#include "usb.h"

static volatile bool g_stop = false;
static void handle_sigint(int) { g_stop = true; }

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

  --dpi SLOT=VALUE         Set a DPI slot (1-5), e.g. --dpi 2=3200
  --led MODE               Set LED mode: off, rainbow, steady, respiration
  --polling-rate HZ        Set USB polling rate: 125, 250, 500, or 1000 (Hz)
  --button NAME=ACTION     Remap a button, e.g. --button side1=f1
                           NAME: side1..12, left, right, middle, fire
                           (run --list-actions for valid action names)

  --list-actions           Print all valid button action names and exit

  --profile N              Target profile 1 or 2 (default: 1; note: the
                           M913 only fully supports profile 1 via USB)

  --raw-send HEX           Send a raw packet and stay in listen mode.
                           HEX = space-separated bytes (up to 16).
                           Bytes are zero-padded to 16; checksum is
                           appended as byte 16 automatically.
                           e.g. --raw-send "08 07 00 00 60 08"

Examples:
  m913-ctl --probe
  m913-ctl --listen
  m913-ctl --config examples/example.ini
  m913-ctl --led rainbow
  m913-ctl --dpi 1=800 --dpi 2=1600 --dpi 3=3200 --dpi 4=6400 --dpi 5=7200
  m913-ctl --button side1=f1 --button side2=f2
  m913-ctl --button fire="fire:50:2"     # fire button: speed=50, repeat=2 times  
  m913-ctl --button side3=media_play --button side4=media_vol_up
  m913-ctl --button side5="ctrl+c" --button side6="a+b"  # key combinations

Note: Run as root or install the udev rule for non-root access:
  sudo cp udev/99-m913.rules /etc/udev/rules.d/
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
// -----------------------------------------------------------------------
static void apply_config(UsbMouse& mouse, const Config& cfg,
                         const uint8_t* btn_layout = nullptr,
                         bool is_compx = false) {

    // ---- Buttons ----
    std::map<uint8_t, ActionBytes> btn_changes;
    for (auto& [key, action_str] : cfg.buttons) {
        Button btn;
        if (!parse_button_name(key, btn)) {
            std::cerr << "  Warning: unknown button '" << key << "', skipping\n";
            continue;
        }
        ActionBytes ab;
        if (!parse_action(action_str, ab)) {
            std::cerr << "  Warning: unknown action '" << action_str
                      << "' for " << key << ", skipping\n";
            continue;
        }
        btn_changes[static_cast<uint8_t>(btn)] = ab;
        // Register multi-key actions for complex parsing
        if (ab[0] == 0x90 && ab[3] > 1) {
            register_multikey_action(static_cast<uint8_t>(btn), action_str);
        }
    }
    if (!btn_changes.empty())
        send_sequence(mouse, build_button_mapping(btn_changes, btn_layout), "Button mapping");

    // ---- DPI ----
    bool any_dpi = false;
    for (int i = 0; i < 5; ++i)
        if (cfg.dpi[i].value != 0) { any_dpi = true; break; }

    if (any_dpi) {
        DpiSettings dpi;
        for (int i = 0; i < 5; ++i) {
            dpi.values[i]  = cfg.dpi[i].value;
            dpi.enabled[i] = cfg.dpi[i].enabled;
        }
        if (is_compx)
            send_sequence(mouse, build_compx_dpi_packets(dpi), "DPI config");
        else
            send_sequence(mouse, build_dpi_packets(dpi), "DPI config");
    }

    // ---- LED ----
    if (is_compx) {
        // Compx has per-slot RGB colors, no global LED modes.
        //   [led] section    → applies one color to every active slot
        //                       (mode=off → black)
        //   dpiN_color keys  → override individual slots, take precedence
        bool any_color = cfg.led.set;
        for (int i = 0; i < 5; ++i)
            if (cfg.dpi[i].color != 0xFFFFFFFF) any_color = true;

        if (any_color) {
            int n_slots = 1;
            for (int i = 0; i < 5; ++i)
                if (cfg.dpi[i].enabled) n_slots = i + 1;

            uint32_t colors[5];
            uint32_t global = cfg.led.set
                ? ((cfg.led.mode == LedMode::Off) ? 0x000000 : cfg.led.color)
                : 0xFFFFFFFF;
            for (int i = 0; i < 5; ++i)
                colors[i] = (cfg.dpi[i].color != 0xFFFFFFFF) ? cfg.dpi[i].color : global;

            send_sequence(mouse, build_compx_color_packets(colors, n_slots), "LED color");
        }
    } else if (cfg.led.set) {
        send_sequence(mouse,
                      build_led_packets(cfg.led.mode, cfg.led.color, cfg.led.brightness, cfg.led.speed),
                      "LED mode");
    }

    // ---- Polling rate ----
    if (cfg.mouse.set)
        send_sequence(mouse,
                      {build_polling_rate_packet(cfg.mouse.polling_rate)},
                      "Polling rate");
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
        {"help",         no_argument,       nullptr, 'h'},
        {"version",      no_argument,       nullptr, 'V'},
        {"listen",          optional_argument, nullptr, 1006},
        {"probe",           no_argument,       nullptr, 1007},
        {"probe-commands",  no_argument,       nullptr, 1008},
        {"raw-send",        required_argument, nullptr, 1009},
        {"debug-device",    no_argument,       nullptr, 1010},
        {"config",       required_argument, nullptr, 'c'},
        {"dpi",          required_argument, nullptr, 1001},
        {"led",          required_argument, nullptr, 1002},
        {"button",       required_argument, nullptr, 1003},
        {"list-actions",  no_argument,       nullptr, 1004},
        {"profile",       required_argument, nullptr, 1005},
        {"polling-rate",  required_argument, nullptr, 1011},
        {"detect-layout", no_argument,       nullptr, 1012},
        {nullptr, 0, nullptr, 0}
    };

    // ---- collect requested operations ----
    bool        do_probe          = false;
    bool        do_listen         = false;
    bool        do_probe_commands = false;
    bool        do_debug_device   = false;
    bool        do_detect_layout  = false;
    int         listen_ep    = -1;  // -1 = auto (try 0x81 and 0x82)
    std::string config_file;
    std::string raw_send_hex;
    Profile     profile      = Profile::P1;

    struct DpiArg  { int slot; uint16_t value; };
    struct BtnArg  { std::string name; std::string action; };

    std::vector<DpiArg>   dpi_args;
    std::vector<BtnArg>   btn_args;
    std::string           led_arg;
    uint16_t              polling_rate_arg = 0;  // 0 = not set

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
                if (slot < 1 || slot > 5) {
                    std::cerr << "Error: DPI slot must be 1-5\n";
                    return 1;
                }
                if (val < 100 || val > 16000 || val % 100 != 0) {
                    std::cerr << "Error: DPI value must be 100-16000 in steps of 100\n";
                    return 1;
                }
                dpi_args.push_back({slot, static_cast<uint16_t>(val)});
            } catch (...) {
                std::cerr << "Error: invalid --dpi argument: " << arg << "\n";
                return 1;
            }
            break;
        }

        case 1002:  // --led MODE
            led_arg = optarg;
            break;

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

        case 1006:  // --listen [EP]
            do_listen = true;
            if (optarg) {
                try {
                    listen_ep = std::stoi(optarg, nullptr, 16);
                } catch (...) {
                    std::cerr << "Error: invalid endpoint '" << optarg
                              << "' (expect hex, e.g. 0x81)\n";
                    return 1;
                }
            }
            break;

        case 1007:  // --probe
            do_probe = true;
            break;

        case 1008:  // --probe-commands
            do_probe_commands = true;
            break;

        case 1009:  // --raw-send HEX
            raw_send_hex = optarg;
            break;

        case 1010:  // --debug-device
            do_debug_device = true;
            break;

        case 1012:  // --detect-layout
            do_detect_layout = true;
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

        case 1005:  // --profile N
            try {
                int p = std::stoi(optarg);
                if (p == 1) profile = Profile::P1;
                else if (p == 2) profile = Profile::P2;
                else {
                    std::cerr << "Error: --profile must be 1 or 2\n";
                    return 1;
                }
            } catch (...) {
                std::cerr << "Error: invalid --profile argument\n";
                return 1;
            }
            break;

        default:
            std::cerr << "Use --help for usage.\n";
            return 1;
        }
    }

    // ---- validate that there's something to do ----
    bool has_work = do_probe || do_probe_commands || do_listen || do_debug_device || do_detect_layout ||
                    !raw_send_hex.empty() ||
                    !config_file.empty() ||
                    !dpi_args.empty() || !led_arg.empty() || !btn_args.empty() ||
                    polling_rate_arg != 0;
    if (!has_work) {
        print_help(argv[0]);
        return 0;
    }

    // ---- --detect-layout: map button indices to physical buttons ----
    if (do_detect_layout) {
        // Letters a-p as unique identifiers for each of the 16 button indices.
        // HID scancodes: a=0x04, b=0x05, ..., p=0x13
        static const char    id_char[16]  = {'a','b','c','d','e','f','g','h',
                                              'i','j','k','l','m','n','o','p'};
        static const uint8_t id_scan[16]  = {0x04,0x05,0x06,0x07,0x08,0x09,
                                              0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
                                              0x10,0x11,0x12,0x13};

        // Friendly names in the order we ask the user to press them.
        static const char* btn_names[16] = {
            "side1","side2","side3","side4","side5","side6",
            "side7","side8","side9","side10","side11","side12",
            "left","right","middle","fire"
        };

        // Step 1: open, remap all 16 indices to unique letters, close.
        {
            UsbMouse dlm;
            bool opened = false;
            const std::vector<std::pair<uint16_t,uint16_t>> candidates = {
                {M913_VID,  M913_PID}, {M913_VID,  M913_PID_WIRED},
                {COMPX_VID, COMPX_PID},{COMPX_VID, COMPX_PID_WIRED},
            };
            uint16_t found_vid = 0, found_pid = 0;
            for (auto [v, p] : candidates) {
                try { dlm.open_all_interfaces(v, p); found_vid=v; found_pid=p; opened=true; break; }
                catch (...) {}
            }
            if (!opened) {
                std::cerr << "Error: no supported device found.\n";
                return 1;
            }
            std::cout << "Connected (" << std::hex
                      << std::setw(4) << std::setfill('0') << found_vid << ":"
                      << std::setw(4) << std::setfill('0') << found_pid
                      << std::dec << ").\n";
            std::cout << "Remapping all 16 button indices to letters a-p...\n";

            std::map<uint8_t, ActionBytes> changes;
            for (uint8_t i = 0; i < 16; ++i)
                changes[i] = {0x90, 0x00, id_scan[i], 0x01};

            auto pkts = build_button_mapping(changes);
            for (auto& p : pkts) send_cmd(dlm, p, "");

            // dlm goes out of scope here → close() reattaches kernel driver
        }

        std::cout << "\nAll buttons remapped. The mouse is now released back to the OS.\n";
        std::cout << "Each physical button now sends a unique letter (a-p).\n\n";
        std::cout << "For each button named below, press the corresponding physical button on the mouse:\n\n";

        // Step 2: interactive — ask for each button name, read the letter.
        // char_to_index: which index did we assign to letter X?
        int char_to_index[256];
        std::fill(char_to_index, char_to_index + 256, -1);
        for (int i = 0; i < 16; ++i)
            char_to_index[static_cast<uint8_t>(id_char[i])] = i;

        int layout[16];  // layout[button_name_idx] = protocol_index
        std::fill(layout, layout + 16, -1);

        // Put terminal in raw mode so single keypresses are captured.
        struct termios orig, raw;
        tcgetattr(STDIN_FILENO, &orig);
        raw = orig;
        raw.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);

        bool aborted = false;
        for (int b = 0; b < 16 && !aborted; ++b) {
            std::cout << "  " << btn_names[b] << ": " << std::flush;
            char ch = 0;
            while (true) {
                if (read(STDIN_FILENO, &ch, 1) < 1) { aborted = true; break; }
                if (ch == 3 || ch == 27) { aborted = true; break; } // Ctrl+C / Esc
                int idx = char_to_index[static_cast<uint8_t>(ch)];
                if (idx >= 0) {
                    std::cout << ch << "\n";
                    layout[b] = idx;
                    break;
                }
                // ignore keys that aren't a-p
            }
        }

        tcsetattr(STDIN_FILENO, TCSANOW, &orig);

        if (aborted) {
            std::cout << "\nAborted.\n";
            return 1;
        }

        std::cout << "\n--- Detected layout ---\n";
        std::cout << "Button name   -> protocol index\n";
        std::cout << std::dec << std::setfill(' ');
        for (int b = 0; b < 16; ++b) {
            std::cout << "  " << std::left << std::setw(10) << btn_names[b]
                      << " -> index " << layout[b] << "\n";
        }
        std::cout << "\nPlease share this output in the GitHub issue.\n";
        std::cout << "\nNOTE: your buttons are still remapped to letters. Reset your mouse\n";
        std::cout << "via Redragon software or apply your config file to restore them.\n";
        return 0;
    }

    // ---- --debug-device: investigate unknown/new hardware ----
    if (do_debug_device) {
        // Known VID/PID combinations to try, in order.
        const std::vector<std::pair<uint16_t,uint16_t>> candidates = {
            {M913_VID,   M913_PID},
            {M913_VID,   M913_PID_WIRED},
            {COMPX_VID,  COMPX_PID},
            {COMPX_VID,  COMPX_PID_WIRED},
        };

        UsbMouse dbg;
        uint16_t found_vid = 0, found_pid = 0;
        for (auto [vid, pid] : candidates) {
            try {
                dbg.open_all_interfaces(vid, pid);
                found_vid = vid; found_pid = pid;
                break;
            } catch (...) {}
        }
        if (!dbg.is_open()) {
            std::cerr << "Error: no known M913 variant found. Is the mouse plugged in?\n";
            return 1;
        }
        std::cout << "=== debug-device ===\n";
        std::cout << "Connected (" << std::hex
                  << std::setw(4) << std::setfill('0') << found_vid << ":"
                  << std::setw(4) << std::setfill('0') << found_pid
                  << std::dec << ").\n\n";

        std::cout << "--- USB descriptor ---\n";
        dbg.probe();
        std::cout << "\n";

        // Listen on all 3 possible endpoints simultaneously for 15 seconds.
        // The user should press all buttons during this time.
        const std::vector<uint8_t> endpoints = {0x81, 0x82, 0x83};
        std::cout << "--- Listening on all endpoints for 15 seconds ---\n";
        std::cout << "Press every mouse button now...\n\n";

        std::mutex print_mtx;
        std::atomic<bool> dbg_stop{false};
        std::vector<std::thread> listeners;

        for (uint8_t ep : endpoints) {
            listeners.emplace_back([&dbg, ep, &print_mtx, &dbg_stop]() {
                uint8_t buf[64] = {};
                while (!dbg_stop) {
                    int got = 0;
                    try { got = dbg.try_recv(buf, sizeof(buf), ep, 200); }
                    catch (...) { break; }
                    if (got > 0) {
                        std::lock_guard<std::mutex> lk(print_mtx);
                        std::cout << "EP 0x" << std::hex << std::setw(2)
                                  << std::setfill('0') << static_cast<int>(ep)
                                  << " (" << std::dec << got << "B): ";
                        std::cout << std::hex << std::setfill('0');
                        for (int i = 0; i < got; ++i)
                            std::cout << std::setw(2) << static_cast<int>(buf[i]) << " ";
                        std::cout << std::dec << "\n";
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::seconds(15));
        dbg_stop = true;
        for (auto& t : listeners) t.join();

        // Send the 0x08 profile-query packet and report the response.
        std::cout << "\n--- Sending 0x08 probe packet (profile query) ---\n";
        static const uint8_t probe_pkt[17] = {
            0x08,0x07,0x00,0x00,0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4b
        };
        try {
            dbg.send(probe_pkt);
            std::cout << "Sent: ";
            std::cout << std::hex << std::setfill('0');
            for (int i = 0; i < 17; ++i)
                std::cout << std::setw(2) << static_cast<int>(probe_pkt[i]) << " ";
            std::cout << std::dec << "\n";

            // Try to receive a response on each endpoint.
            for (uint8_t ep : endpoints) {
                uint8_t resp[64] = {};
                int got = dbg.try_recv(resp, sizeof(resp), ep, 500);
                if (got > 0) {
                    std::cout << "Response on EP 0x" << std::hex
                              << std::setw(2) << std::setfill('0')
                              << static_cast<int>(ep)
                              << " (" << std::dec << got << "B): ";
                    std::cout << std::hex << std::setfill('0');
                    for (int i = 0; i < got; ++i)
                        std::cout << std::setw(2) << static_cast<int>(resp[i]) << " ";
                    std::cout << std::dec << "\n";
                }
            }
        } catch (const std::exception& e) {
            std::cout << "Send failed: " << e.what() << "\n";
            std::cout << "(This may mean the protocol or interface index is different)\n";
        }

        std::cout << "\n--- Done. Please share the full output above. ---\n";
        return 0;
    }

    // ---- open mouse ----
    UsbMouse mouse;
    const uint8_t* btn_layout = nullptr;
    bool           is_compx   = false;
    try {
        uint16_t vid = M913_VID, pid = M913_PID;
        const std::vector<std::pair<uint16_t,uint16_t>> candidates = {
            {M913_VID,  M913_PID},
            {M913_VID,  M913_PID_WIRED},
            {COMPX_VID, COMPX_PID},
            {COMPX_VID, COMPX_PID_WIRED},
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
            std::signal(SIGINT, handle_sigint);
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
            std::signal(SIGINT, handle_sigint);

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

        // ---- --config FILE ----
        bool did_config = false;

        if (!config_file.empty()) {
            std::cout << "=== Applying config: " << config_file << " ===\n";
            Config cfg = parse_config_file(config_file);
            cfg.profile = profile;
            validate_config(cfg);
            apply_config(mouse, cfg, btn_layout, is_compx);
            did_config = true;
        }

        // ---- inline --dpi args ----
        if (!dpi_args.empty()) {
            DpiSettings dpi;
            for (auto& [slot, val] : dpi_args) {
                if (slot >= 1 && slot <= 5)
                    dpi.values[slot - 1] = val;
            }
            if (is_compx)
                send_sequence(mouse, build_compx_dpi_packets(dpi), "DPI config");
            else
                send_sequence(mouse, build_dpi_packets(dpi), "DPI config");
            did_config = true;
        }

        // ---- inline --led arg ----
        if (!led_arg.empty()) {
            LedMode mode;
            std::string sl = led_arg;
            for (auto& c : sl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if      (sl == "off")       mode = LedMode::Off;
            else if (sl == "rainbow")   mode = LedMode::Rainbow;
            else if (sl == "static")    mode = LedMode::Steady;
            else if (sl == "steady")    mode = LedMode::Steady;
            else if (sl == "breathing") mode = LedMode::Respiration;
            else if (sl == "respiration") mode = LedMode::Respiration;
            else {
                std::cerr << "Error: unknown LED mode '" << led_arg
                          << "'. Valid: off, rainbow, steady, respiration\n";
                exit_code = 1;
                goto cleanup;
            }
            if (is_compx) {
                uint32_t slot_color = (mode == LedMode::Off) ? 0x000000 : 0x00ff00;
                uint32_t colors[5] = {slot_color, slot_color, slot_color, slot_color, slot_color};
                send_sequence(mouse, build_compx_color_packets(colors, 5), "LED color");
            } else {
                send_sequence(mouse, build_led_packets(mode), "LED mode");
            }
            did_config = true;
        }

        // ---- inline --button args ----
        if (!btn_args.empty()) {
            std::map<uint8_t, ActionBytes> btn_changes;
            for (auto& [name, action_str] : btn_args) {
                Button btn;
                if (!parse_button_name(name, btn)) {
                    std::cerr << "Error: unknown button name '" << name << "'\n";
                    exit_code = 1;
                    goto cleanup;
                }
                ActionBytes ab;
                if (!parse_action(action_str, ab)) {
                    std::cerr << "Error: unknown action '" << action_str << "'\n";
                    exit_code = 1;
                    goto cleanup;
                }
                btn_changes[static_cast<uint8_t>(btn)] = ab;
                // Register multi-key actions for complex parsing
                if (ab[0] == 0x90 && ab[3] > 1) {
                    register_multikey_action(static_cast<uint8_t>(btn), action_str);
                }
            }
            send_sequence(mouse,
                          build_button_mapping(btn_changes, btn_layout),
                          "Button mapping");
            did_config = true;
        }

        // ---- inline --polling-rate arg ----
        if (polling_rate_arg != 0) {
            send_sequence(mouse,
                          {build_polling_rate_packet(polling_rate_arg)},
                          "Polling rate");
            did_config = true;
        }

        // ---- commit ----
        // The Redragon software always ends a config session with two
        // "08 04 00..." packets (observed in USB captures).  These appear
        // to act as a commit/apply-to-flash command.
        if (did_config) {
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
