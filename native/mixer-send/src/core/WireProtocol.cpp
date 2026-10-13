// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/WireProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mixersend::wire
{

namespace
{
    // --- little-endian primitives ---------------------------------------
    inline void put8 (std::uint8_t*& p, std::uint8_t v) noexcept { *p++ = v; }

    inline void put16 (std::uint8_t*& p, std::uint16_t v) noexcept
    {
        p[0] = static_cast<std::uint8_t> (v);
        p[1] = static_cast<std::uint8_t> (v >> 8);
        p += 2;
    }

    inline void put32 (std::uint8_t*& p, std::uint32_t v) noexcept
    {
        for (int i = 0; i < 4; ++i)
            p[i] = static_cast<std::uint8_t> (v >> (8 * i));
        p += 4;
    }

    inline void put64 (std::uint8_t*& p, std::uint64_t v) noexcept
    {
        for (int i = 0; i < 8; ++i)
            p[i] = static_cast<std::uint8_t> (v >> (8 * i));
        p += 8;
    }

    struct Reader
    {
        const std::uint8_t* p;
        const std::uint8_t* end;

        std::size_t remaining() const noexcept { return static_cast<std::size_t> (end - p); }

        std::uint8_t u8() noexcept { return *p++; }

        std::uint16_t u16() noexcept
        {
            const auto v = static_cast<std::uint16_t> (p[0] | (p[1] << 8));
            p += 2;
            return v;
        }

        std::uint32_t u32() noexcept
        {
            std::uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
                v |= static_cast<std::uint32_t> (p[i]) << (8 * i);
            p += 4;
            return v;
        }

        std::uint64_t u64() noexcept
        {
            std::uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v |= static_cast<std::uint64_t> (p[i]) << (8 * i);
            p += 8;
            return v;
        }
    };

    bool isKnownType (std::uint8_t t) noexcept
    {
        switch (static_cast<MessageType> (t))
        {
            case MessageType::hello:
            case MessageType::info:
            case MessageType::state:
            case MessageType::bye:
            case MessageType::welcome:
            case MessageType::control:
            case MessageType::lease:
            case MessageType::reassign:
                return true;
        }
        return false;
    }

    bool hasTarget (MessageType t) noexcept
    {
        return t == MessageType::welcome || t == MessageType::control
            || t == MessageType::lease || t == MessageType::reassign;
    }

    /** Length of the UTF-8 sequence starting at `p` if it is well-formed
        (no overlongs, no surrogates, <= U+10FFFF), else 0. */
    std::size_t utf8SequenceLength (const std::uint8_t* p, std::size_t available) noexcept
    {
        const std::uint8_t b0 = p[0];

        if (b0 < 0x80)
            return 1;

        std::size_t length = 0;
        std::uint32_t cp = 0;
        std::uint32_t minimum = 0;

        if ((b0 & 0xE0) == 0xC0) { length = 2; cp = b0 & 0x1F; minimum = 0x80; }
        else if ((b0 & 0xF0) == 0xE0) { length = 3; cp = b0 & 0x0F; minimum = 0x800; }
        else if ((b0 & 0xF8) == 0xF0) { length = 4; cp = b0 & 0x07; minimum = 0x10000; }
        else return 0;

        if (length > available)
            return 0;

        for (std::size_t i = 1; i < length; ++i)
        {
            if ((p[i] & 0xC0) != 0x80)
                return 0;
            cp = (cp << 6) | (p[i] & 0x3F);
        }

        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;

        return length;
    }

    bool validState (const StateRecord& r) noexcept
    {
        return r.instanceId != 0 && r.volume14 <= kVolume14Max && r.pan >= -100 && r.pan <= 100
            && (r.flags & 0xF0) == 0;
    }

    bool validControl (const ControlRecord& r) noexcept
    {
        if (r.instanceId == 0 || r.seq == 0 || r.mask == 0 || (r.mask & ~controlMask::all) != 0)
            return false;

        const bool hasVolume = (r.mask & controlMask::volume) != 0;
        const bool hasPan = (r.mask & controlMask::pan) != 0;
        const bool hasMute = (r.mask & controlMask::mute) != 0;

        if (hasVolume ? r.volume14 > kVolume14Max : r.volume14 != 0)
            return false;
        if (hasPan ? (r.pan < -100 || r.pan > 100) : r.pan != 0)
            return false;
        if (! hasMute && r.muted)
            return false;
        return true;
    }

    bool validInfo (const InfoRecord& r) noexcept
    {
        return r.instanceId != 0 && (r.channels == 1 || r.channels == 2)
            && (r.hasColour || r.colourArgb == 0) && r.name.length <= kMaxNameBytes
            && isValidWireText (reinterpret_cast<const std::uint8_t*> (r.name.bytes), r.name.length);
    }

    bool validLease (const LeaseRecord& r) noexcept
    {
        return r.instanceId != 0 && r.durationMs <= kMaxLeaseMs;
    }

    bool validHello (const Hello& h) noexcept
    {
        return static_cast<std::uint8_t> (h.format) <= static_cast<std::uint8_t> (Format::standalone)
            && h.tickHz >= 1 && h.tickHz <= 120 && h.hostName.length <= kMaxNameBytes
            && isValidWireText (reinterpret_cast<const std::uint8_t*> (h.hostName.bytes), h.hostName.length);
    }
}

//==============================================================================
bool isValidWireText (const std::uint8_t* bytes, std::size_t length) noexcept
{
    for (std::size_t i = 0; i < length;)
    {
        if (bytes[i] < 0x20 || bytes[i] == 0x7F)
            return false;

        const auto n = utf8SequenceLength (bytes + i, length - i);

        if (n == 0)
            return false;

        i += n;
    }
    return true;
}

void ShortText::assignSanitised (std::string_view text) noexcept
{
    length = 0;
    const auto* p = reinterpret_cast<const std::uint8_t*> (text.data());
    std::size_t i = 0;

    while (i < text.size())
    {
        auto n = utf8SequenceLength (p + i, text.size() - i);

        if (n == 0) // drop a malformed byte
        {
            ++i;
            continue;
        }

        if (length + n > kMaxNameBytes)
            break;

        if (n == 1 && (p[i] < 0x20 || p[i] == 0x7F))
            bytes[length++] = ' ';
        else
            for (std::size_t k = 0; k < n; ++k)
                bytes[length++] = static_cast<char> (p[i + k]);

        i += n;
    }

    // Trim surrounding whitespace so a host that pads names does not show as
    // a different name.
    std::size_t start = 0;
    while (start < length && bytes[start] == ' ')
        ++start;
    while (length > start && bytes[length - 1] == ' ')
        --length;

    if (start > 0)
    {
        std::memmove (bytes, bytes + start, length - start);
        length = static_cast<std::uint8_t> (length - start);
    }

    bytes[length] = '\0';
}

bool ShortText::assignExact (std::string_view text) noexcept
{
    if (text.size() > kMaxNameBytes
        || ! isValidWireText (reinterpret_cast<const std::uint8_t*> (text.data()), text.size()))
        return false;

    std::memcpy (bytes, text.data(), text.size());
    length = static_cast<std::uint8_t> (text.size());
    bytes[length] = '\0';
    return true;
}

void Message::clearRecords() noexcept
{
    info.clear();
    states.clear();
    controls.clear();
    leases.clear();
    ids.clear();
    targetBusId = 0;
    hello = {};
}

const char* toString (DecodeError e) noexcept
{
    switch (e)
    {
        case DecodeError::none: return "none";
        case DecodeError::magic: return "magic";
        case DecodeError::version: return "version";
        case DecodeError::type: return "type";
        case DecodeError::length: return "length";
        case DecodeError::range: return "range";
    }
    return "unknown";
}

//==============================================================================
DecodeError decode (const std::uint8_t* data, std::size_t size, Message& out)
{
    out.clearRecords();

    if (data == nullptr || size < kHeaderBytes)
        return DecodeError::length;

    if (std::memcmp (data, kMagic, 4) != 0)
        return DecodeError::magic;

    if (data[4] != kVersion)
        return DecodeError::version;

    if (! isKnownType (data[5]))
        return DecodeError::type;

    Reader r { data + 6, data + size };
    const auto type = static_cast<MessageType> (data[5]);
    const auto count = r.u16();
    const auto sender = r.u64();

    if (sender == 0)
        return DecodeError::range;

    out.type = type;
    out.senderId = sender;

    if (hasTarget (type))
    {
        if (r.remaining() < kTargetBytes)
            return DecodeError::length;

        out.targetBusId = r.u64();
    }

    const auto exactly = [&r] (std::size_t recordBytes, std::size_t records) {
        return r.remaining() == recordBytes * records;
    };

    switch (type)
    {
        case MessageType::hello:
        {
            if (count != 0 || r.remaining() < kHelloFixedBytes)
                return DecodeError::length;

            auto& h = out.hello;
            h.pid = r.u32();
            h.pluginVersion = r.u32();
            const auto format = r.u8();
            h.tickHz = r.u8();
            h.instanceCount = r.u16();
            const auto nameLength = r.u8();

            if (nameLength > kMaxNameBytes)
                return DecodeError::range;
            if (r.remaining() != nameLength)
                return DecodeError::length;
            if (format > static_cast<std::uint8_t> (Format::standalone) || h.tickHz < 1 || h.tickHz > 120)
                return DecodeError::range;
            if (! isValidWireText (r.p, nameLength))
                return DecodeError::range;

            h.format = static_cast<Format> (format);
            std::memcpy (h.hostName.bytes, r.p, nameLength);
            h.hostName.length = nameLength;
            h.hostName.bytes[nameLength] = '\0';
            return DecodeError::none;
        }

        case MessageType::info:
        {
            if (count == 0)
                return DecodeError::length;

            for (std::uint16_t i = 0; i < count; ++i)
            {
                if (r.remaining() < kInfoFixedBytes)
                    return DecodeError::length;

                InfoRecord rec;
                rec.instanceId = r.u64();
                rec.channels = r.u8();
                const auto flags = r.u8();
                rec.colourArgb = r.u32();
                const auto nameLength = r.u8();
                rec.hasColour = (flags & 0x01) != 0;

                if (nameLength > kMaxNameBytes)
                    return DecodeError::range;
                if (r.remaining() < nameLength)
                    return DecodeError::length;
                if (rec.instanceId == 0 || (rec.channels != 1 && rec.channels != 2) || (flags & ~0x01) != 0
                    || (! rec.hasColour && rec.colourArgb != 0) || ! isValidWireText (r.p, nameLength))
                    return DecodeError::range;

                std::memcpy (rec.name.bytes, r.p, nameLength);
                rec.name.length = nameLength;
                rec.name.bytes[nameLength] = '\0';
                r.p += nameLength;
                out.info.push_back (rec);
            }

            return r.remaining() == 0 ? DecodeError::none : DecodeError::length;
        }

        case MessageType::state:
        {
            if (count == 0 || ! exactly (kStateRecordBytes, count))
                return DecodeError::length;

            for (std::uint16_t i = 0; i < count; ++i)
            {
                StateRecord rec;
                rec.instanceId = r.u64();
                rec.volume14 = r.u16();
                rec.pan = static_cast<std::int8_t> (r.u8());
                rec.flags = r.u8();
                rec.peakL = r.u8();
                rec.peakR = r.u8();
                rec.rmsL = r.u8();
                rec.rmsR = r.u8();
                rec.ackSeq = r.u16();

                if (! validState (rec))
                    return DecodeError::range;

                out.states.push_back (rec);
            }
            return DecodeError::none;
        }

        case MessageType::bye:
        case MessageType::reassign:
        {
            if ((type == MessageType::reassign && count == 0) || ! exactly (kIdBytes, count))
                return DecodeError::length;
            if (type == MessageType::reassign && out.targetBusId == 0)
                return DecodeError::range;

            for (std::uint16_t i = 0; i < count; ++i)
            {
                const auto id = r.u64();
                if (id == 0)
                    return DecodeError::range;
                out.ids.push_back (id);
            }
            return DecodeError::none;
        }

        case MessageType::welcome:
        {
            if (count != 0 || r.remaining() != 0)
                return DecodeError::length;
            return out.targetBusId != 0 ? DecodeError::none : DecodeError::range;
        }

        case MessageType::control:
        {
            if (count == 0 || ! exactly (kControlRecordBytes, count))
                return DecodeError::length;
            if (out.targetBusId == 0)
                return DecodeError::range;

            for (std::uint16_t i = 0; i < count; ++i)
            {
                ControlRecord rec;
                rec.instanceId = r.u64();
                rec.seq = r.u16();
                rec.mask = r.u8();
                rec.volume14 = r.u16();
                rec.pan = static_cast<std::int8_t> (r.u8());
                const auto muted = r.u8();

                if (muted > 1)
                    return DecodeError::range;

                rec.muted = muted == 1;

                if (! validControl (rec))
                    return DecodeError::range;

                out.controls.push_back (rec);
            }
            return DecodeError::none;
        }

        case MessageType::lease:
        {
            if (count == 0 || ! exactly (kLeaseRecordBytes, count))
                return DecodeError::length;
            if (out.targetBusId == 0)
                return DecodeError::range;

            for (std::uint16_t i = 0; i < count; ++i)
            {
                LeaseRecord rec;
                rec.instanceId = r.u64();
                const auto flags = r.u8();
                rec.durationMs = r.u16();

                if ((flags & ~0x01) != 0)
                    return DecodeError::range;

                rec.mute = (flags & 0x01) != 0;

                if (! validLease (rec))
                    return DecodeError::range;

                out.leases.push_back (rec);
            }
            return DecodeError::none;
        }
    }

    return DecodeError::type;
}

//==============================================================================
void DatagramBuilder::setCapacity (std::size_t bytes) noexcept
{
    capacity = std::clamp<std::size_t> (bytes, kHeaderBytes + kTargetBytes + kStateRecordBytes, kMaxPluginDatagramBytes);
}

void DatagramBuilder::begin (MessageType type, std::uint64_t senderId) noexcept
{
    auto* p = buffer;
    std::memcpy (p, kMagic, 4);
    p += 4;
    put8 (p, kVersion);
    put8 (p, static_cast<std::uint8_t> (type));
    put16 (p, 0);
    put64 (p, senderId);
    used = kHeaderBytes;
    recordCount = 0;
}

void DatagramBuilder::beginWithTarget (MessageType type, std::uint64_t senderId, std::uint64_t targetBusId) noexcept
{
    begin (type, senderId);
    auto* p = buffer + used;
    put64 (p, targetBusId);
    used += kTargetBytes;
}

bool DatagramBuilder::reserve (std::size_t bytes) noexcept
{
    return used + bytes <= capacity && recordCount < 0xFFFF;
}

void DatagramBuilder::patchCount() noexcept
{
    buffer[6] = static_cast<std::uint8_t> (recordCount);
    buffer[7] = static_cast<std::uint8_t> (recordCount >> 8);
}

bool DatagramBuilder::setHello (const Hello& h) noexcept
{
    if (! validHello (h) || ! reserve (kHelloFixedBytes + h.hostName.length))
        return false;

    auto* p = buffer + used;
    put32 (p, h.pid);
    put32 (p, h.pluginVersion);
    put8 (p, static_cast<std::uint8_t> (h.format));
    put8 (p, h.tickHz);
    put16 (p, h.instanceCount);
    put8 (p, h.hostName.length);
    std::memcpy (p, h.hostName.bytes, h.hostName.length);
    used += kHelloFixedBytes + h.hostName.length;
    return true;
}

bool DatagramBuilder::add (const InfoRecord& rec) noexcept
{
    if (! validInfo (rec) || ! reserve (kInfoFixedBytes + rec.name.length))
        return false;

    auto* p = buffer + used;
    put64 (p, rec.instanceId);
    put8 (p, rec.channels);
    put8 (p, rec.hasColour ? 0x01 : 0x00);
    put32 (p, rec.hasColour ? rec.colourArgb : 0);
    put8 (p, rec.name.length);
    std::memcpy (p, rec.name.bytes, rec.name.length);
    used += kInfoFixedBytes + rec.name.length;
    ++recordCount;
    patchCount();
    return true;
}

bool DatagramBuilder::add (const StateRecord& rec) noexcept
{
    if (! validState (rec) || ! reserve (kStateRecordBytes))
        return false;

    auto* p = buffer + used;
    put64 (p, rec.instanceId);
    put16 (p, rec.volume14);
    put8 (p, static_cast<std::uint8_t> (rec.pan));
    put8 (p, rec.flags);
    put8 (p, rec.peakL);
    put8 (p, rec.peakR);
    put8 (p, rec.rmsL);
    put8 (p, rec.rmsR);
    put16 (p, rec.ackSeq);
    used += kStateRecordBytes;
    ++recordCount;
    patchCount();
    return true;
}

bool DatagramBuilder::add (const ControlRecord& rec) noexcept
{
    if (! validControl (rec) || ! reserve (kControlRecordBytes))
        return false;

    auto* p = buffer + used;
    put64 (p, rec.instanceId);
    put16 (p, rec.seq);
    put8 (p, rec.mask);
    put16 (p, rec.volume14);
    put8 (p, static_cast<std::uint8_t> (rec.pan));
    put8 (p, rec.muted ? 1 : 0);
    used += kControlRecordBytes;
    ++recordCount;
    patchCount();
    return true;
}

bool DatagramBuilder::add (const LeaseRecord& rec) noexcept
{
    if (! validLease (rec) || ! reserve (kLeaseRecordBytes))
        return false;

    auto* p = buffer + used;
    put64 (p, rec.instanceId);
    put8 (p, rec.mute ? 0x01 : 0x00);
    put16 (p, rec.durationMs);
    used += kLeaseRecordBytes;
    ++recordCount;
    patchCount();
    return true;
}

bool DatagramBuilder::addId (std::uint64_t instanceId) noexcept
{
    if (instanceId == 0 || ! reserve (kIdBytes))
        return false;

    auto* p = buffer + used;
    put64 (p, instanceId);
    used += kIdBytes;
    ++recordCount;
    patchCount();
    return true;
}

//==============================================================================
bool encode (const Message& m, std::vector<std::uint8_t>& out, std::size_t capacity)
{
    // Tests and tools only: a generous buffer, same writers as the bus.
    std::vector<std::uint8_t> scratch (std::max<std::size_t> (capacity, kHeaderBytes + kTargetBytes), 0);
    std::uint8_t* const base = scratch.data();
    std::uint8_t* p = base;
    const std::uint8_t* const end = base + scratch.size();

    const auto fits = [&] (std::size_t n) { return static_cast<std::size_t> (end - p) >= n; };

    if (m.senderId == 0 || ! fits (kHeaderBytes))
        return false;

    std::memcpy (p, kMagic, 4);
    p += 4;
    put8 (p, kVersion);
    put8 (p, static_cast<std::uint8_t> (m.type));
    std::uint8_t* countAt = p;
    put16 (p, 0);
    put64 (p, m.senderId);

    std::size_t count = 0;

    if (hasTarget (m.type))
    {
        if (m.targetBusId == 0 || ! fits (kTargetBytes))
            return false;
        put64 (p, m.targetBusId);
    }

    switch (m.type)
    {
        case MessageType::hello:
        {
            const auto& h = m.hello;
            if (! validHello (h) || ! fits (kHelloFixedBytes + h.hostName.length))
                return false;
            put32 (p, h.pid);
            put32 (p, h.pluginVersion);
            put8 (p, static_cast<std::uint8_t> (h.format));
            put8 (p, h.tickHz);
            put16 (p, h.instanceCount);
            put8 (p, h.hostName.length);
            std::memcpy (p, h.hostName.bytes, h.hostName.length);
            p += h.hostName.length;
            break;
        }

        case MessageType::info:
            if (m.info.empty())
                return false;
            for (const auto& rec : m.info)
            {
                if (! validInfo (rec) || ! fits (kInfoFixedBytes + rec.name.length))
                    return false;
                put64 (p, rec.instanceId);
                put8 (p, rec.channels);
                put8 (p, rec.hasColour ? 0x01 : 0x00);
                put32 (p, rec.hasColour ? rec.colourArgb : 0);
                put8 (p, rec.name.length);
                std::memcpy (p, rec.name.bytes, rec.name.length);
                p += rec.name.length;
            }
            count = m.info.size();
            break;

        case MessageType::state:
            if (m.states.empty())
                return false;
            for (const auto& rec : m.states)
            {
                if (! validState (rec) || ! fits (kStateRecordBytes))
                    return false;
                put64 (p, rec.instanceId);
                put16 (p, rec.volume14);
                put8 (p, static_cast<std::uint8_t> (rec.pan));
                put8 (p, rec.flags);
                put8 (p, rec.peakL);
                put8 (p, rec.peakR);
                put8 (p, rec.rmsL);
                put8 (p, rec.rmsR);
                put16 (p, rec.ackSeq);
            }
            count = m.states.size();
            break;

        case MessageType::bye:
        case MessageType::reassign:
            if (m.type == MessageType::reassign && m.ids.empty())
                return false;
            for (const auto id : m.ids)
            {
                if (id == 0 || ! fits (kIdBytes))
                    return false;
                put64 (p, id);
            }
            count = m.ids.size();
            break;

        case MessageType::welcome:
            break;

        case MessageType::control:
            if (m.controls.empty())
                return false;
            for (const auto& rec : m.controls)
            {
                if (! validControl (rec) || ! fits (kControlRecordBytes))
                    return false;
                put64 (p, rec.instanceId);
                put16 (p, rec.seq);
                put8 (p, rec.mask);
                put16 (p, rec.volume14);
                put8 (p, static_cast<std::uint8_t> (rec.pan));
                put8 (p, rec.muted ? 1 : 0);
            }
            count = m.controls.size();
            break;

        case MessageType::lease:
            if (m.leases.empty())
                return false;
            for (const auto& rec : m.leases)
            {
                if (! validLease (rec) || ! fits (kLeaseRecordBytes))
                    return false;
                put64 (p, rec.instanceId);
                put8 (p, rec.mute ? 0x01 : 0x00);
                put16 (p, rec.durationMs);
            }
            count = m.leases.size();
            break;
    }

    if (count > 0xFFFF)
        return false;

    put16 (countAt, static_cast<std::uint16_t> (count));
    out.assign (base, p);
    return true;
}

//==============================================================================
std::uint8_t meterCodeFromAmplitude (float amplitude) noexcept
{
    if (! (amplitude > 1.0e-5f)) // -100 dBFS, and NaN
        return 0;

    const double decibels = 20.0 * std::log10 (static_cast<double> (amplitude));
    const double code = std::round ((decibels + 100.0) * 2.0);
    return static_cast<std::uint8_t> (std::clamp (code, 0.0, 255.0));
}

double meterCodeToDecibels (std::uint8_t code) noexcept
{
    return code / 2.0 - 100.0;
}

bool isNewerSeq (std::uint16_t seq, std::uint16_t last) noexcept
{
    const auto diff = static_cast<std::uint16_t> (seq - last);
    return diff != 0 && diff < 0x8000;
}

} // namespace mixersend::wire
