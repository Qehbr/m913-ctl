#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "config.h"

// -----------------------------------------------------------------------
// Configuration read-back
//
// The device answers a read (byte[1] = 0x08, bytes[3..4] = address,
// byte[5] = 0x0a) with 10 payload bytes in [6..15] at that address. The
// addresses are the same ones the write templates use, so decoding is a
// direct mapping back onto them — see M913_READ_CODES in protocol.h for
// where the address list comes from and what has been verified on hardware.
//
// This file is the decoding half only, and deliberately touches no USB: the
// reading itself lives in main.cpp next to the other transfers. That keeps
// decode_device_config() a pure function over a byte map, so the round-trip
// test in tests/regress.sh can feed it an image built by the packet builders
// and check that what the tool writes is what it reads back — no device, and
// no libusb to link.
// -----------------------------------------------------------------------

// 10-byte payload chunks, keyed by the device address of the first byte.
using BlockMap = std::map<uint16_t, std::array<uint8_t, 10>>;

// Bytes per read chunk, as issued by M913_READ_CODES.
static constexpr size_t READ_CHUNK = 10;

struct DecodeReport {
    // Addresses a decode needed but the BlockMap did not hold.
    std::vector<uint16_t> missing;
    // Human-readable notes: fields whose stored bytes made no sense, buttons
    // that could not be named, and so on.
    std::vector<std::string> warnings;
    // Buttons whose 4 stored bytes have no known action name, as
    // "button_side3" → "04 3a 09 0e". Written out as commented-out INI lines
    // so a saved file stays applicable but still shows what was there.
    std::map<std::string, std::string> unnamed_buttons;
};

// Decode a read-back image into a Config.
//
// Areson layout only. The Compx revision answers a different report type at
// addresses that have never been captured, so there is nothing to decode
// there yet and callers must not offer this for VID 3554.
//
// Returns false if nothing could be decoded at all (an empty or unusable
// image); a partial decode returns true with the gaps listed in `report`.
bool decode_device_config(const BlockMap& blocks, Config& out, DecodeReport& report);

// True if a device→host reply carries the checksum the protocol specifies:
// (0x4C - sum(bytes[1..15])) & 0xFF, with byte[0] (report ID 0x09) excluded.
bool verify_reply_checksum(const uint8_t pkt[M913_PACKET_SIZE]);

// True if `reply` is the device's answer to `request`.
//
// EP 0x82 carries HID input reports and the answers to *earlier* requests as
// well as this one's, so a caller that takes whatever turns up next will
// sooner or later credit one packet's acknowledgement to another. That is not
// hypothetical: it is how a dropped write was observed being reported as
// successful, with the bytes never reaching the device.
//
// The device echoes a request's header back with report ID 0x09 in place of
// 0x08, so sub-command and address identify the answer. The payload is NOT
// compared: a write's acknowledgement echoes it, but a commit's carries status
// bytes instead, so an equality test on the whole packet would reject the
// commit acknowledgements every config session ends with.
bool ack_matches(const Packet& request, const uint8_t reply[M913_PACKET_SIZE]);

// The read requests a decode needs, in order: the read entries of
// M913_READ_CODES (sub-command 0x08, 10-byte chunk) below `addr_limit`.
//
// The handshake and commit entries are not reads, and the 16 regions at
// 0x0301+ hold nothing decodable — they read as erased flash and nothing is
// known to live there (see M913_READ_CODES in protocol.h). Derived from the
// table rather than from hardcoded indices, so a read code added there cannot
// be silently left unfetched.
std::vector<const Packet*> config_read_requests(uint16_t addr_limit = 0x0300);

// The address a read request asks about: bytes [3..4], big-endian.
uint16_t request_address(const Packet& req);
