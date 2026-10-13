// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

/*  The Mixer SEND UDP wire, protocol v1 -- the C++ mirror of
    packages/protocol/src/mixersend.ts. The byte layout is specified in
    docs/MIXER_SEND.md and pinned by packages/protocol/fixtures/
    mixersend-golden.json, which the tests of BOTH codecs check.

    Little-endian throughout, integers only, one message per datagram. A
    decoder rejects a whole datagram (never part of it) on any inconsistency:
    unknown magic / version / type, a length that is not exactly what the
    header and the records imply, or any field out of range, reserved bits
    included. */

namespace mixersend::wire
{

inline constexpr std::uint8_t kMagic[4] = { 'M', 'X', 'S', 'N' };
inline constexpr std::uint8_t kVersion = 1;

inline constexpr std::size_t kHeaderBytes = 16;
inline constexpr std::size_t kTargetBytes = 8;
/** Companion -> plugin, and the plugin's fallback: fits any MTU. */
inline constexpr std::size_t kMaxDatagramBytes = 1400;
/** Plugin -> companion on loopback (MTU 16384, macOS UDP cap 9216): 128
    instances' state fits one datagram, so one sendto per tick. */
inline constexpr std::size_t kMaxPluginDatagramBytes = 8192;
inline constexpr std::size_t kMaxNameBytes = 63;
inline constexpr std::size_t kHelloFixedBytes = 13;
inline constexpr std::size_t kInfoFixedBytes = 15;
inline constexpr std::size_t kStateRecordBytes = 18;
inline constexpr std::size_t kControlRecordBytes = 15;
inline constexpr std::size_t kLeaseRecordBytes = 11;
inline constexpr std::size_t kIdBytes = 8;

inline constexpr std::uint16_t kVolume14Max = 16383;
inline constexpr std::uint16_t kMaxLeaseMs = 60000;
inline constexpr std::uint16_t kDefaultCompanionPort = 4891;

enum class MessageType : std::uint8_t
{
    hello = 0x01,
    info = 0x02,
    state = 0x03,
    bye = 0x04,
    welcome = 0x10,
    control = 0x11,
    lease = 0x12,
    reassign = 0x13,
};

enum class Format : std::uint8_t
{
    unknown = 0,
    vst3 = 1,
    au = 2,
    auv3 = 3,
    standalone = 4,
};

namespace controlMask
{
    inline constexpr std::uint8_t volume = 0x01;
    inline constexpr std::uint8_t pan = 0x02;
    inline constexpr std::uint8_t mute = 0x04;
    inline constexpr std::uint8_t all = volume | pan | mute;
}

namespace stateFlag
{
    inline constexpr std::uint8_t muted = 0x01;
    inline constexpr std::uint8_t leaseMuted = 0x02;
    inline constexpr std::uint8_t processing = 0x04;
    inline constexpr std::uint8_t bypassed = 0x08;
}

/** Bounded UTF-8 text without heap allocation (names and host names). */
struct ShortText
{
    char bytes[kMaxNameBytes + 1] {};
    std::uint8_t length = 0;

    std::string_view view() const noexcept { return { bytes, length }; }

    /** Stores `text` as a valid, printable, <= 63-byte UTF-8 string: invalid
        sequences are dropped, control characters become spaces, and the cut
        never splits a code point. */
    void assignSanitised (std::string_view text) noexcept;

    /** Stores `text` verbatim if it is valid wire text; false otherwise. */
    bool assignExact (std::string_view text) noexcept;
};

struct Hello
{
    std::uint32_t pid = 0;
    std::uint32_t pluginVersion = 0; ///< major << 16 | minor << 8 | patch
    Format format = Format::unknown;
    std::uint8_t tickHz = 30;
    std::uint16_t instanceCount = 0;
    ShortText hostName;
};

struct InfoRecord
{
    std::uint64_t instanceId = 0;
    std::uint8_t channels = 2;
    bool hasColour = false;
    std::uint32_t colourArgb = 0;
    ShortText name; ///< empty = the host gave no name
};

struct StateRecord
{
    std::uint64_t instanceId = 0;
    std::uint16_t volume14 = 0;
    std::int8_t pan = 0;
    std::uint8_t flags = 0;
    std::uint8_t peakL = 0, peakR = 0, rmsL = 0, rmsR = 0;
    std::uint16_t ackSeq = 0;
};

struct ControlRecord
{
    std::uint64_t instanceId = 0;
    std::uint16_t seq = 0;
    std::uint8_t mask = 0;
    std::uint16_t volume14 = 0;
    std::int8_t pan = 0;
    bool muted = false;
};

struct LeaseRecord
{
    std::uint64_t instanceId = 0;
    bool mute = false;
    std::uint16_t durationMs = 0;
};

/** One decoded datagram. Only the members that belong to `type` are
    meaningful; the vectors keep their capacity across reuse, so a receiver
    that holds one Message allocates only while its largest datagram grows. */
struct Message
{
    MessageType type = MessageType::hello;
    std::uint64_t senderId = 0;
    std::uint64_t targetBusId = 0; ///< companion -> plugin messages only
    Hello hello;
    std::vector<InfoRecord> info;
    std::vector<StateRecord> states;
    std::vector<ControlRecord> controls;
    std::vector<LeaseRecord> leases;
    std::vector<std::uint64_t> ids; ///< bye, reassign

    void clearRecords() noexcept;
};

enum class DecodeError
{
    none,
    magic,
    version,
    type,
    length,
    range,
};

const char* toString (DecodeError) noexcept;

/** Decodes one datagram into `out`. Never throws on any input. */
DecodeError decode (const std::uint8_t* data, std::size_t size, Message& out);

/** Encodes `message` as ONE datagram (tests and tools). Returns false if a
    field is out of range or it does not fit `capacity`. */
bool encode (const Message& message, std::vector<std::uint8_t>& out, std::size_t capacity = 65507);

/*  Builds datagrams record by record into a fixed buffer, for the bus: no
    allocation, and a record that does not fit returns false so the caller
    can flush and start the next datagram. */
class DatagramBuilder
{
public:
    void begin (MessageType type, std::uint64_t senderId) noexcept;
    void beginWithTarget (MessageType type, std::uint64_t senderId, std::uint64_t targetBusId) noexcept;

    bool setHello (const Hello& hello) noexcept;
    bool add (const InfoRecord& record) noexcept;
    bool add (const StateRecord& record) noexcept;
    bool add (const ControlRecord& record) noexcept;
    bool add (const LeaseRecord& record) noexcept;
    bool addId (std::uint64_t instanceId) noexcept;

    std::uint16_t count() const noexcept { return recordCount; }
    const std::uint8_t* data() const noexcept { return buffer; }
    std::size_t size() const noexcept { return used; }

    void setCapacity (std::size_t bytes) noexcept;

private:
    bool reserve (std::size_t bytes) noexcept;
    void patchCount() noexcept;

    std::uint8_t buffer[kMaxPluginDatagramBytes] {};
    std::size_t capacity = kMaxDatagramBytes;
    std::size_t used = 0;
    std::uint16_t recordCount = 0;
};

/** Meter code: half-dB steps above -100 dBFS (0 = at or below -100 dBFS). */
std::uint8_t meterCodeFromAmplitude (float amplitude) noexcept;
double meterCodeToDecibels (std::uint8_t code) noexcept;

/** True if `seq` is newer than `last` in 16-bit serial-number arithmetic. */
bool isNewerSeq (std::uint16_t seq, std::uint16_t last) noexcept;

/** UTF-8 check used by the decoder: well-formed, no control characters. */
bool isValidWireText (const std::uint8_t* bytes, std::size_t length) noexcept;

} // namespace mixersend::wire
