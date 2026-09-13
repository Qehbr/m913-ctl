#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include <libusb.h>

// Redragon M913 USB identifiers — original hardware (Areson, VID 25a7)
static constexpr uint16_t M913_VID       = 0x25a7;
static constexpr uint16_t M913_PID       = 0xfa07;  // 2.4G wireless receiver
static constexpr uint16_t M913_PID_WIRED = 0xfa08;  // wired / dual-mode USB

// Redragon M913 newer hardware (Compx, VID 3554)
static constexpr uint16_t COMPX_VID       = 0x3554;
static constexpr uint16_t COMPX_PID       = 0xf55d;  // 2.4G wireless receiver
static constexpr uint16_t COMPX_PID_WIRED = 0xf55e;  // wired / 3-mode USB

// Packet size for all M913 control/interrupt transfers
static constexpr int M913_PACKET_SIZE = 17;

// Control transfer parameters (host → device)
static constexpr uint8_t  CTRL_REQUEST_TYPE = 0x21;  // host-to-device, class, interface
static constexpr uint8_t  CTRL_REQUEST      = 0x09;  // HID SET_REPORT
static constexpr uint16_t CTRL_VALUE        = 0x0308;
static constexpr uint16_t CTRL_INDEX        = 0x0001;

// Interrupt IN endpoint (device → host)
static constexpr uint8_t INTERRUPT_EP_IN = 0x82;

// Timeout for USB transfers in milliseconds
static constexpr unsigned int USB_TIMEOUT_MS = 2000;

class UsbMouse {
public:
    UsbMouse();
    ~UsbMouse();

    // Non-copyable
    UsbMouse(const UsbMouse&) = delete;
    UsbMouse& operator=(const UsbMouse&) = delete;

    // Open the device and claim every interface it exposes, detaching the
    // kernel driver from each. Interface count comes from the descriptor:
    // the Compx revision has three, the Areson two.
    void open_all_interfaces(uint16_t vid, uint16_t pid);

    // Override the HID report type used in SET_REPORT control transfers.
    // Areson hardware: 0x0308 (feature report). Compx hardware: 0x0208 (output report).
    void set_ctrl_value(uint16_t v) { _ctrl_value = v; }

    // Close and reattach kernel driver
    void close();

    // Send a 17-byte configuration packet to the mouse
    // Throws std::runtime_error on failure
    void send(const uint8_t data[M913_PACKET_SIZE]);

    // Read whatever arrives on an interrupt endpoint, returning 0 on timeout
    // rather than throwing — a timeout is the normal case here, since the
    // mouse only answers when it has something to say.
    //
    // This takes the NEXT packet on the endpoint, which is not necessarily a
    // reply to anything: EP 0x82 also carries HID input reports while the
    // mouse is in use. Callers that need the answer to a specific request
    // must match it themselves — see fetch_block() in readback.cpp.
    //
    // Returns the number of bytes received. buf must hold buf_size bytes.
    int try_recv(uint8_t* buf, int buf_size, uint8_t endpoint = INTERRUPT_EP_IN,
                 unsigned int timeout_ms = 500);

    // Print all USB interfaces and endpoints for this device to stdout.
    void probe();

    bool is_open() const { return _handle != nullptr; }

private:
    libusb_context*       _ctx    = nullptr;
    libusb_device_handle* _handle = nullptr;

    bool     _detached_iface0 = false;
    bool     _detached_iface1 = false;
    bool     _detached_iface2 = false;
    int      _num_interfaces   = 2;
    uint16_t _ctrl_value       = CTRL_VALUE;

    void _claim_interface(int iface, bool& detached_flag);
    void _release_interface(int iface, bool detached_flag);
};
