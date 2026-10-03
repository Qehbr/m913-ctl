#include "usb.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

UsbMouse::UsbMouse() {
    int r = libusb_init(&_ctx);
    if (r < 0) {
        throw std::runtime_error(
            std::string("libusb_init failed: ") + libusb_strerror(static_cast<libusb_error>(r)));
    }
}

UsbMouse::~UsbMouse() {
    close();
    if (_ctx) {
        libusb_exit(_ctx);
        _ctx = nullptr;
    }
}

void UsbMouse::close() {
    if (!_handle) return;

    _release_interface(0, _detached_iface0);
    _release_interface(1, _detached_iface1);
    if (_num_interfaces > 2)
        _release_interface(2, _detached_iface2);

    libusb_close(_handle);
    _handle = nullptr;
}

void UsbMouse::open_all_interfaces(uint16_t vid, uint16_t pid) {
    _handle = libusb_open_device_with_vid_pid(_ctx, vid, pid);
    if (!_handle) {
        throw std::runtime_error(
            "Could not find or open device " +
            [&]() {
                std::ostringstream ss;
                ss << std::hex << std::setw(4) << std::setfill('0') << vid
                   << ":" << std::setw(4) << std::setfill('0') << pid;
                return ss.str();
            }() +
            " — is the mouse plugged in? Try running with sudo.");
    }

    // Discover how many interfaces the device has, then claim all of them.
    // While the descriptor is open, record every IN endpoint's packet size —
    // try_recv() needs it to size its reads legally (see the note there).
    libusb_device* dev = libusb_get_device(_handle);
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) == 0) {
        _num_interfaces = cfg->bNumInterfaces;
        for (int i = 0; i < cfg->bNumInterfaces; ++i)
            for (int a = 0; a < cfg->interface[i].num_altsetting; ++a) {
                const auto& alt = cfg->interface[i].altsetting[a];
                for (int e = 0; e < alt.bNumEndpoints; ++e)
                    _max_packet[alt.endpoint[e].bEndpointAddress] =
                        alt.endpoint[e].wMaxPacketSize;
            }
        libusb_free_config_descriptor(cfg);
    }

    _claim_interface(0, _detached_iface0);
    _claim_interface(1, _detached_iface1);
    if (_num_interfaces > 2)
        _claim_interface(2, _detached_iface2);
}

void UsbMouse::send(const uint8_t data[M913_PACKET_SIZE]) {
    // libusb_control_transfer expects a non-const data pointer for OUT transfers
    // (it won't modify it, but the API isn't const-correct)
    uint8_t buf[M913_PACKET_SIZE];
    std::memcpy(buf, data, M913_PACKET_SIZE);

    int r = libusb_control_transfer(
        _handle,
        CTRL_REQUEST_TYPE,
        CTRL_REQUEST,
        _ctrl_value,
        CTRL_INDEX,
        buf,
        M913_PACKET_SIZE,
        USB_TIMEOUT_MS);

    if (r < 0) {
        throw std::runtime_error(
            std::string("Control transfer (send) failed: ") +
            libusb_strerror(static_cast<libusb_error>(r)));
    }
    // libusb_control_transfer returns the number of bytes actually
    // transferred on success (not necessarily all of them) — a short
    // write here previously went unnoticed, and callers waiting for a
    // device ACK would blame the resulting timeout on device latency
    // rather than a truncated SET_REPORT.
    //
    // This throws where a missing ACK only warns (send_cmd() in main.cpp).
    // The asymmetry is deliberate: a missing ACK means the outcome is
    // unknown — the link is slow and the write may well have landed —
    // whereas a short write means the packet demonstrably did not arrive.
    if (r != M913_PACKET_SIZE) {
        std::ostringstream oss;
        oss << "Control transfer (send) wrote only " << r << " of "
            << M913_PACKET_SIZE << " bytes";
        throw std::runtime_error(oss.str());
    }
}

int UsbMouse::try_recv(uint8_t* buf, int buf_size, uint8_t endpoint,
                       unsigned int timeout_ms) {
    // An interrupt read's length must be an exact multiple of the endpoint's
    // wMaxPacketSize, or the transfer fails with LIBUSB_ERROR_OVERFLOW the
    // moment the device actually sends something.
    //
    // This is not a guess. Measured on 25a7:fa07 against EP 0x81, whose
    // wMaxPacketSize is 7: sizes 7, 14, 21, 28, 63 and 70 all read a packet,
    // while 8, 13, 16, 17, 64 and 128 all threw. The failure is invisible
    // until traffic arrives, which is why a too-large buffer looks fine on an
    // idle device and then breaks the moment the mouse is moved.
    //
    // So the request is rounded DOWN to whole packets here rather than left to
    // each caller. A plain `uint8_t buf[64]` is the natural thing to write and
    // is wrong on both of this device's endpoints (64 divides neither 7 nor
    // 17); rounding centrally means no call site has to know that. Only
    // buf_size bytes are ever touched, so this can only ever read less.
    if (buf_size > 0) {
        auto it = _max_packet.find(endpoint);
        if (it != _max_packet.end() && it->second > 0) {
            int whole = (buf_size / it->second) * it->second;
            if (whole > 0) buf_size = whole;
        }
    }

    int transferred = 0;
    int r = libusb_interrupt_transfer(
        _handle,
        endpoint,
        buf,
        buf_size,
        &transferred,
        timeout_ms);

    if (r == LIBUSB_ERROR_TIMEOUT) return 0;
    if (r < 0) {
        throw std::runtime_error(
            std::string("Interrupt transfer failed on EP 0x") +
            [endpoint]{ std::ostringstream s; s << std::hex << static_cast<int>(endpoint); return s.str(); }() +
            ": " + libusb_strerror(static_cast<libusb_error>(r)));
    }
    return transferred;
}

void UsbMouse::probe() {
    libusb_device* dev = libusb_get_device(_handle);
    libusb_config_descriptor* cfg = nullptr;

    if (libusb_get_active_config_descriptor(dev, &cfg) < 0) {
        std::cout << "Could not get config descriptor\n";
        return;
    }

    std::cout << "USB descriptor: " << static_cast<int>(cfg->bNumInterfaces)
              << " interface(s)\n";

    for (int i = 0; i < cfg->bNumInterfaces; ++i) {
        const auto& iface = cfg->interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a) {
            const auto& alt = iface.altsetting[a];
            std::cout << "  Interface " << static_cast<int>(alt.bInterfaceNumber)
                      << " (class " << static_cast<int>(alt.bInterfaceClass)
                      << ", subclass " << static_cast<int>(alt.bInterfaceSubClass)
                      << ", protocol " << static_cast<int>(alt.bInterfaceProtocol)
                      << ")  endpoints: " << static_cast<int>(alt.bNumEndpoints) << "\n";
            for (int e = 0; e < alt.bNumEndpoints; ++e) {
                const auto& ep = alt.endpoint[e];
                uint8_t addr  = ep.bEndpointAddress;
                uint8_t attrs = ep.bmAttributes;
                std::string dir  = (addr & 0x80) ? "IN " : "OUT";
                std::string type;
                switch (attrs & 0x03) {
                    case 0: type = "Control";   break;
                    case 1: type = "Isochronous"; break;
                    case 2: type = "Bulk";      break;
                    case 3: type = "Interrupt"; break;
                }
                std::cout << std::hex << std::setfill('0');
                std::cout << "    EP 0x" << std::setw(2) << static_cast<int>(addr)
                          << "  " << dir << "  " << type
                          << "  maxPacket=" << std::dec << ep.wMaxPacketSize
                          << "  interval=" << static_cast<int>(ep.bInterval) << "ms\n";
            }
        }
    }

    libusb_free_config_descriptor(cfg);
}

// --- private helpers ---

void UsbMouse::_claim_interface(int iface, bool& detached_flag) {
    detached_flag = false;

    if (libusb_kernel_driver_active(_handle, iface) == 1) {
        int r = libusb_detach_kernel_driver(_handle, iface);
        if (r < 0) {
            throw std::runtime_error(
                "Failed to detach kernel driver from interface " +
                std::to_string(iface) + ": " +
                libusb_strerror(static_cast<libusb_error>(r)));
        }
        detached_flag = true;
    }

    int r = libusb_claim_interface(_handle, iface);
    if (r < 0) {
        throw std::runtime_error(
            "Failed to claim interface " + std::to_string(iface) + ": " +
            libusb_strerror(static_cast<libusb_error>(r)));
    }
}

void UsbMouse::_release_interface(int iface, bool /*detached_flag*/) {
    libusb_release_interface(_handle, iface);

    // Always try to reattach, not only when this process did the detaching.
    //
    // If a previous run was killed before it could clean up (SIGKILL, a crash,
    // or — before stop handlers covered SIGTERM — any plain `kill`), the
    // interface is left with no driver bound and the mouse stops working.
    // Reattaching only when _detached_flag was set made that state permanent:
    // the next run sees no active driver, so it never sets the flag, so it
    // never reattaches either. Replugging was the only way out.
    //
    // Attaching when a driver is already bound just returns LIBUSB_ERROR_BUSY,
    // and LIBUSB_ERROR_NOT_FOUND if there is nothing to attach, so ignoring
    // the result is safe and makes every run self-healing.
    libusb_attach_kernel_driver(_handle, iface);
}
