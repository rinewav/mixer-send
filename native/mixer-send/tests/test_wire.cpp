// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "test_framework.h"

#include "core/InstanceId.h"
#include "core/WireProtocol.h"

#include <juce_core/juce_core.h>

#include <random>

using namespace mixersend;
using namespace mixersend::wire;

namespace
{
    std::vector<std::uint8_t> fromHex (const juce::String& hex)
    {
        std::vector<std::uint8_t> bytes;
        for (int i = 0; i + 1 < hex.length(); i += 2)
            bytes.push_back (static_cast<std::uint8_t> (hex.substring (i, i + 2).getHexValue32()));
        return bytes;
    }

    juce::String toHex (const std::vector<std::uint8_t>& bytes)
    {
        juce::String s;
        for (auto b : bytes)
            s << juce::String::toHexString (static_cast<int> (b)).paddedLeft ('0', 2);
        return s;
    }

    std::uint64_t idOf (const juce::var& v)
    {
        return idFromHex (v.toString().toStdString()).value_or (0);
    }

    std::string text (const ShortText& t) { return std::string (t.view()); }

    MessageType typeFromName (const juce::String& name)
    {
        if (name == "hello") return MessageType::hello;
        if (name == "info") return MessageType::info;
        if (name == "state") return MessageType::state;
        if (name == "bye") return MessageType::bye;
        if (name == "welcome") return MessageType::welcome;
        if (name == "control") return MessageType::control;
        if (name == "lease") return MessageType::lease;
        return MessageType::reassign;
    }

    Format formatFromName (const juce::String& name)
    {
        if (name == "vst3") return Format::vst3;
        if (name == "au") return Format::au;
        if (name == "auv3") return Format::auv3;
        if (name == "standalone") return Format::standalone;
        return Format::unknown;
    }

    std::uint32_t versionFrom (const juce::String& s)
    {
        juce::StringArray parts;
        parts.addTokens (s, ".", "");
        return static_cast<std::uint32_t> ((parts[0].getIntValue() << 16) | (parts[1].getIntValue() << 8) | parts[2].getIntValue());
    }

    /** The fixture's JSON object -> a C++ Message, independently of decode(). */
    Message messageFromJson (const juce::var& m)
    {
        Message out;
        out.type = typeFromName (m["type"]);
        out.senderId = idOf (m["senderId"]);
        out.targetBusId = m.hasProperty ("targetBusId") ? idOf (m["targetBusId"]) : 0;

        switch (out.type)
        {
            case MessageType::hello:
                out.hello.pid = static_cast<std::uint32_t> (static_cast<juce::int64> (m["pid"]));
                out.hello.pluginVersion = versionFrom (m["pluginVersion"]);
                out.hello.format = formatFromName (m["format"]);
                out.hello.tickHz = static_cast<std::uint8_t> (static_cast<int> (m["tickHz"]));
                out.hello.instanceCount = static_cast<std::uint16_t> (static_cast<int> (m["instanceCount"]));
                out.hello.hostName.assignExact (m["hostName"].toString().toStdString());
                break;

            case MessageType::info:
                for (const auto& r : *m["instances"].getArray())
                {
                    InfoRecord rec;
                    rec.instanceId = idOf (r["instanceId"]);
                    rec.channels = static_cast<std::uint8_t> (static_cast<int> (r["channels"]));
                    rec.hasColour = r.hasProperty ("colour");
                    rec.colourArgb = rec.hasColour ? static_cast<std::uint32_t> (r["colour"].toString().substring (1).getHexValue64()) : 0;
                    if (r.hasProperty ("name"))
                        rec.name.assignExact (r["name"].toString().toStdString());
                    out.info.push_back (rec);
                }
                break;

            case MessageType::state:
                for (const auto& r : *m["instances"].getArray())
                {
                    StateRecord rec;
                    rec.instanceId = idOf (r["instanceId"]);
                    rec.volume14 = static_cast<std::uint16_t> (static_cast<int> (r["volume14"]));
                    rec.pan = static_cast<std::int8_t> (static_cast<int> (r["pan"]));
                    rec.flags = static_cast<std::uint8_t> ((static_cast<bool> (r["muted"]) ? stateFlag::muted : 0)
                                                         | (static_cast<bool> (r["leaseMuted"]) ? stateFlag::leaseMuted : 0)
                                                         | (static_cast<bool> (r["processing"]) ? stateFlag::processing : 0)
                                                         | (static_cast<bool> (r["bypassed"]) ? stateFlag::bypassed : 0));
                    rec.peakL = static_cast<std::uint8_t> (static_cast<int> (r["peakL"]));
                    rec.peakR = static_cast<std::uint8_t> (static_cast<int> (r["peakR"]));
                    rec.rmsL = static_cast<std::uint8_t> (static_cast<int> (r["rmsL"]));
                    rec.rmsR = static_cast<std::uint8_t> (static_cast<int> (r["rmsR"]));
                    rec.ackSeq = static_cast<std::uint16_t> (static_cast<int> (r["ackSeq"]));
                    out.states.push_back (rec);
                }
                break;

            case MessageType::bye:
            case MessageType::reassign:
                for (const auto& id : *m["instanceIds"].getArray())
                    out.ids.push_back (idOf (id));
                break;

            case MessageType::welcome:
                break;

            case MessageType::control:
                for (const auto& r : *m["controls"].getArray())
                {
                    ControlRecord rec;
                    rec.instanceId = idOf (r["instanceId"]);
                    rec.seq = static_cast<std::uint16_t> (static_cast<int> (r["seq"]));
                    if (r.hasProperty ("volume14"))
                    {
                        rec.mask |= controlMask::volume;
                        rec.volume14 = static_cast<std::uint16_t> (static_cast<int> (r["volume14"]));
                    }
                    if (r.hasProperty ("pan"))
                    {
                        rec.mask |= controlMask::pan;
                        rec.pan = static_cast<std::int8_t> (static_cast<int> (r["pan"]));
                    }
                    if (r.hasProperty ("muted"))
                    {
                        rec.mask |= controlMask::mute;
                        rec.muted = r["muted"];
                    }
                    out.controls.push_back (rec);
                }
                break;

            case MessageType::lease:
                for (const auto& r : *m["leases"].getArray())
                {
                    LeaseRecord rec;
                    rec.instanceId = idOf (r["instanceId"]);
                    rec.mute = r["mute"];
                    rec.durationMs = static_cast<std::uint16_t> (static_cast<int> (r["durationMs"]));
                    out.leases.push_back (rec);
                }
                break;
        }

        return out;
    }

    bool sameMessage (const Message& a, const Message& b)
    {
        if (a.type != b.type || a.senderId != b.senderId || a.targetBusId != b.targetBusId)
            return false;

        switch (a.type)
        {
            case MessageType::hello:
                return a.hello.pid == b.hello.pid && a.hello.pluginVersion == b.hello.pluginVersion
                    && a.hello.format == b.hello.format && a.hello.tickHz == b.hello.tickHz
                    && a.hello.instanceCount == b.hello.instanceCount && text (a.hello.hostName) == text (b.hello.hostName);

            case MessageType::info:
                if (a.info.size() != b.info.size())
                    return false;
                for (std::size_t i = 0; i < a.info.size(); ++i)
                {
                    const auto& x = a.info[i];
                    const auto& y = b.info[i];
                    if (x.instanceId != y.instanceId || x.channels != y.channels || x.hasColour != y.hasColour
                        || x.colourArgb != y.colourArgb || text (x.name) != text (y.name))
                        return false;
                }
                return true;

            case MessageType::state:
                if (a.states.size() != b.states.size())
                    return false;
                for (std::size_t i = 0; i < a.states.size(); ++i)
                {
                    const auto& x = a.states[i];
                    const auto& y = b.states[i];
                    if (x.instanceId != y.instanceId || x.volume14 != y.volume14 || x.pan != y.pan || x.flags != y.flags
                        || x.peakL != y.peakL || x.peakR != y.peakR || x.rmsL != y.rmsL || x.rmsR != y.rmsR || x.ackSeq != y.ackSeq)
                        return false;
                }
                return true;

            case MessageType::bye:
            case MessageType::reassign:
                return a.ids == b.ids;

            case MessageType::welcome:
                return true;

            case MessageType::control:
                if (a.controls.size() != b.controls.size())
                    return false;
                for (std::size_t i = 0; i < a.controls.size(); ++i)
                {
                    const auto& x = a.controls[i];
                    const auto& y = b.controls[i];
                    if (x.instanceId != y.instanceId || x.seq != y.seq || x.mask != y.mask || x.volume14 != y.volume14
                        || x.pan != y.pan || x.muted != y.muted)
                        return false;
                }
                return true;

            case MessageType::lease:
                if (a.leases.size() != b.leases.size())
                    return false;
                for (std::size_t i = 0; i < a.leases.size(); ++i)
                    if (a.leases[i].instanceId != b.leases[i].instanceId || a.leases[i].mute != b.leases[i].mute
                        || a.leases[i].durationMs != b.leases[i].durationMs)
                        return false;
                return true;
        }
        return false;
    }
}

TEST (golden_vectors_decode_reencode_and_reject_exactly_like_the_fixture)
{
    const juce::File file (juce::String (test::fixturePath ("mixersend-golden.json")));
    const auto fixture = juce::JSON::parse (file.loadFileAsString());
    EXPECT (static_cast<int> (fixture["version"]) == kVersion);

    int valid = 0, invalid = 0;

    for (const auto& vector : *fixture["vectors"].getArray())
    {
        const auto name = vector["name"].toString().toStdString();
        const auto bytes = fromHex (vector["hex"]);
        Message decoded;
        const auto error = decode (bytes.data(), bytes.size(), decoded);

        if (vector.hasProperty ("error"))
        {
            ++invalid;
            EXPECT_MSG (error != DecodeError::none, name);
            EXPECT_MSG (vector["error"].toString() == toString (error), name + ": got " + toString (error));
            continue;
        }

        ++valid;
        EXPECT_MSG (error == DecodeError::none, name + ": " + toString (error));

        const auto expected = messageFromJson (vector["message"]);
        EXPECT_MSG (sameMessage (decoded, expected), name + ": decoded differs from fixture");

        std::vector<std::uint8_t> encoded;
        EXPECT_MSG (encode (expected, encoded), name + ": encode refused");
        EXPECT_MSG (toHex (encoded) == vector["hex"].toString(), name + ": re-encoded bytes differ");
    }

    EXPECT (valid == 13);
    EXPECT (invalid == 31);
}

TEST (builder_splits_many_records_into_datagrams_that_all_decode)
{
    DatagramBuilder builder; // default capacity: the conservative 1400 bytes
    std::vector<std::vector<std::uint8_t>> datagrams;
    std::size_t total = 0;

    builder.begin (MessageType::state, 42);

    for (int i = 1; i <= 300; ++i)
    {
        StateRecord record;
        record.instanceId = static_cast<std::uint64_t> (i);
        record.volume14 = static_cast<std::uint16_t> (i * 50);
        record.pan = static_cast<std::int8_t> (i % 201 - 100);

        if (! builder.add (record))
        {
            datagrams.emplace_back (builder.data(), builder.data() + builder.size());
            builder.begin (MessageType::state, 42);
            EXPECT (builder.add (record));
        }
    }
    datagrams.emplace_back (builder.data(), builder.data() + builder.size());

    std::uint64_t expectId = 1;
    for (const auto& d : datagrams)
    {
        EXPECT (d.size() <= kMaxDatagramBytes);
        Message m;
        EXPECT (decode (d.data(), d.size(), m) == DecodeError::none);
        for (const auto& s : m.states)
            EXPECT (s.instanceId == expectId++);
        total += m.states.size();
    }

    EXPECT (total == 300);
    EXPECT (datagrams.size() == 4); // 76 records of 18 bytes fit in 1400

    // The plugin's own capacity on loopback: 128 instances in ONE datagram.
    DatagramBuilder big;
    big.setCapacity (kMaxPluginDatagramBytes);
    big.begin (MessageType::state, 42);
    for (int i = 1; i <= 128; ++i)
    {
        StateRecord record;
        record.instanceId = static_cast<std::uint64_t> (i);
        EXPECT (big.add (record));
    }
    EXPECT (big.size() == kHeaderBytes + 128 * kStateRecordBytes);
    Message m;
    EXPECT (decode (big.data(), big.size(), m) == DecodeError::none && m.states.size() == 128);
}

TEST (builder_refuses_out_of_range_records)
{
    DatagramBuilder builder;
    builder.begin (MessageType::state, 1);
    StateRecord bad;
    bad.instanceId = 5;
    bad.volume14 = 16384;
    EXPECT (! builder.add (bad));
    bad.volume14 = 0;
    bad.pan = 101;
    EXPECT (! builder.add (bad));
    EXPECT (builder.count() == 0);
}

TEST (random_and_mutated_bytes_never_crash_the_decoder)
{
    std::mt19937 rng (1234);
    Message m;
    int accepted = 0;

    // Pure noise.
    for (int i = 0; i < 200000; ++i)
    {
        std::vector<std::uint8_t> bytes (rng() % 64);
        for (auto& b : bytes)
            b = static_cast<std::uint8_t> (rng());
        if (decode (bytes.data(), bytes.size(), m) == DecodeError::none)
            ++accepted;
    }

    // Valid control datagrams with one byte flipped: must decode or reject, never misbehave.
    Message control;
    control.type = MessageType::control;
    control.senderId = 9;
    control.targetBusId = 7;
    control.controls.push_back ({ 3, 1, controlMask::all, 12451, -20, true });
    std::vector<std::uint8_t> good;
    EXPECT (encode (control, good));

    for (int i = 0; i < 100000; ++i)
    {
        auto bytes = good;
        bytes[rng() % bytes.size()] ^= static_cast<std::uint8_t> (1u << (rng() % 8));
        if (rng() % 4 == 0)
            bytes.resize (rng() % (bytes.size() + 4), 0);
        decode (bytes.data(), bytes.size(), m);
    }

    EXPECT (decode (nullptr, 0, m) == DecodeError::length);
    EXPECT (accepted == 0); // noise essentially never carries the magic
}

TEST (meter_codes_and_sequence_arithmetic)
{
    EXPECT (meterCodeFromAmplitude (0.0f) == 0);
    EXPECT (meterCodeFromAmplitude (std::nanf ("")) == 0);
    EXPECT (meterCodeFromAmplitude (1.0f) == 200);         // 0 dBFS
    EXPECT (meterCodeFromAmplitude (0.5f) == 188);         // -6.02 dBFS
    EXPECT (meterCodeFromAmplitude (1000.0f) == 255);      // clamped
    EXPECT_NEAR (meterCodeToDecibels (200), 0.0, 0.0);
    EXPECT_NEAR (meterCodeToDecibels (0), -100.0, 0.0);

    EXPECT (isNewerSeq (2, 1));
    EXPECT (! isNewerSeq (1, 2));
    EXPECT (! isNewerSeq (5, 5));
    EXPECT (isNewerSeq (1, 65535));   // wrap
    EXPECT (! isNewerSeq (65535, 1));
}

TEST (names_are_sanitised_to_valid_bounded_utf8)
{
    ShortText t;
    t.assignSanitised ("  Kick\tIn  ");
    EXPECT (text (t) == "Kick In");

    t.assignSanitised (std::string ("Bad\xC3\x28Name"));
    EXPECT (text (t) == "Bad(Name");

    // 30 x "ボ" (3 bytes each) = 90 bytes: cut at a code point, never inside one.
    std::string longName;
    for (int i = 0; i < 30; ++i)
        longName += "ボ";
    t.assignSanitised (longName);
    EXPECT (t.length == 63);
    EXPECT (isValidWireText (reinterpret_cast<const std::uint8_t*> (t.bytes), t.length));

    EXPECT (! t.assignExact ("line\nbreak"));
    EXPECT (t.assignExact ("ボーカル"));
}

TEST (instance_ids_are_random_nonzero_and_hex_round_trip)
{
    for (int i = 0; i < 1000; ++i)
    {
        const auto id = randomNonZeroId();
        EXPECT (id != 0);
        EXPECT (idFromHex (idToHex (id)) == id);
    }

    EXPECT (idToHex (0x0123456789abcdefull) == "0123456789abcdef");
    EXPECT (! idFromHex ("0000000000000000").has_value());
    EXPECT (! idFromHex ("123").has_value());
    EXPECT (! idFromHex ("zz23456789abcdef").has_value());
}
