#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include <libusb.h>

// Redragon M913 USB identifiers
static constexpr uint16_t M913_VID = 0x25a7;
static constexpr uint16_t M913_PID = 0xfa07;   // 2.4G wireless receiver

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

    // Open the mouse by VID/PID (detaches kernel driver automatically)
    void open(uint16_t vid = M913_VID, uint16_t pid = M913_PID);

    // Close and reattach kernel driver
    void close();

    // Send a 17-byte configuration packet to the mouse
    // Throws std::runtime_error on failure
    void send(const uint8_t data[M913_PACKET_SIZE]);

    // Receive a 17-byte response from the mouse via interrupt transfer
    // Throws std::runtime_error on failure
    void recv(uint8_t data[M913_PACKET_SIZE]);

    // Send a packet and read back the response (combined operation)
    void send_recv(const uint8_t tx[M913_PACKET_SIZE], uint8_t rx[M913_PACKET_SIZE]);

    // Like recv(), but returns false on timeout instead of throwing.
    // Returns the number of bytes actually received (0 on timeout).
    // buf must be at least buf_size bytes. Used by --listen mode.
    int try_recv(uint8_t* buf, int buf_size, uint8_t endpoint = INTERRUPT_EP_IN,
                 unsigned int timeout_ms = 500);

    // Print all USB interfaces and endpoints for this device to stdout.
    void probe();

    bool is_open() const { return _handle != nullptr; }

private:
    libusb_context*       _ctx    = nullptr;
    libusb_device_handle* _handle = nullptr;

    bool _detached_iface0 = false;
    bool _detached_iface1 = false;

    void _claim_interface(int iface, bool& detached_flag);
    void _release_interface(int iface, bool detached_flag);
};