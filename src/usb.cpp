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

void UsbMouse::open(uint16_t vid, uint16_t pid) {
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
            " — is the mouse plugged in? Try running with sudo or install the udev rule.");
    }

    // The mouse exposes two interfaces that need to be claimed:
    //   Interface 0: mouse (movement, clicks)
    //   Interface 1: keyboard/extra buttons (config channel lives here)
    _claim_interface(0, _detached_iface0);
    _claim_interface(1, _detached_iface1);
}

void UsbMouse::close() {
    if (!_handle) return;

    _release_interface(0, _detached_iface0);
    _release_interface(1, _detached_iface1);

    libusb_close(_handle);
    _handle = nullptr;
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
        CTRL_VALUE,
        CTRL_INDEX,
        buf,
        M913_PACKET_SIZE,
        USB_TIMEOUT_MS);

    if (r < 0) {
        throw std::runtime_error(
            std::string("Control transfer (send) failed: ") +
            libusb_strerror(static_cast<libusb_error>(r)));
    }
}

void UsbMouse::recv(uint8_t data[M913_PACKET_SIZE]) {
    int transferred = 0;
    int r = libusb_interrupt_transfer(
        _handle,
        INTERRUPT_EP_IN,
        data,
        M913_PACKET_SIZE,
        &transferred,
        USB_TIMEOUT_MS);

    if (r < 0) {
        throw std::runtime_error(
            std::string("Interrupt transfer (recv) failed: ") +
            libusb_strerror(static_cast<libusb_error>(r)));
    }
    if (transferred != M913_PACKET_SIZE) {
        throw std::runtime_error(
            "Incomplete receive: got " + std::to_string(transferred) +
            " bytes, expected " + std::to_string(M913_PACKET_SIZE));
    }
}

void UsbMouse::send_recv(const uint8_t tx[M913_PACKET_SIZE], uint8_t rx[M913_PACKET_SIZE]) {
    send(tx);
    recv(rx);
}

int UsbMouse::try_recv(uint8_t* buf, int buf_size, uint8_t endpoint,
                       unsigned int timeout_ms) {
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

void UsbMouse::_release_interface(int iface, bool detached_flag) {
    libusb_release_interface(_handle, iface);
    if (detached_flag) {
        libusb_attach_kernel_driver(_handle, iface);
    }
}