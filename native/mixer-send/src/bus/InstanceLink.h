// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "core/GainStage.h"
#include "core/WireProtocol.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>

namespace mixersend
{

/*  Everything one plugin instance shares with the bus, and who may touch what.

    The processor owns its link as a plain member and registers it with the
    bus for its whole life. The bus reaches a link ONLY while holding its
    registry lock, and the processor unregisters (taking that same lock) at
    the very start of its destructor. So once unregister returns, the bus
    thread can never touch the link, the parameters it points at, or the
    processor's callback again: no use-after-free on removal, whatever the
    bus thread was doing at that moment.

    Thread ownership:
      audio thread   writes meters, bypassed; reads leaseMuted (atomics only)
      bus thread     writes leaseMuted, the mailbox; reads parameters, meters,
                     info, ackSeq; owns `bus` (under the registry lock)
      message thread writes info, ackSeq; drains the mailbox
    The audio thread never takes a lock and never waits on anyone. */
struct InstanceLink
{
    using Clock = std::chrono::steady_clock;

    /** What the companion asked for, waiting for the message thread. */
    struct RemoteChange
    {
        std::uint8_t mask = 0; ///< wire::controlMask bits
        std::uint16_t volume14 = 0;
        std::int8_t pan = 0;
        bool muted = false;
        std::uint16_t seq = 0;
    };

    // --- identity -------------------------------------------------------
    std::atomic<std::uint64_t> id { 0 };

    // --- host-facing parameters, read by the bus (must outlive registration)
    const std::atomic<float>* volumePosition = nullptr; ///< 0..1 fader travel
    const std::atomic<float>* pan = nullptr;            ///< -100..+100
    const std::atomic<float>* mute = nullptr;           ///< 0 or 1

    // --- audio thread -> bus -------------------------------------------
    MeterHandoff meters;
    std::atomic<int> numChannels { 2 };
    std::atomic<bool> bypassed { false };

    // --- bus -> audio thread -------------------------------------------
    /** Mute imposed by a companion lease (solo). Never saved, always expires. */
    std::atomic<bool> leaseMuted { false };

    // --- message thread -> bus -----------------------------------------
    /** seq of the last control applied to the host parameters. */
    std::atomic<std::uint16_t> ackSeq { 0 };
    std::atomic<bool> infoDirty { true };

    /** Called by the bus thread (under the registry lock) after it posted a
        remote change. The processor turns it into a message-thread callback. */
    std::function<void()> onRemoteChange;

    // --- track name / colour (message thread writes, bus reads) --------
    void setTrackProperties (std::optional<std::string_view> name, std::optional<std::uint32_t> colourArgb)
    {
        {
            const std::lock_guard lock (infoMutex);
            if (name.has_value())
                trackName.assignSanitised (*name);
            else
                trackName = {};
            hasColour = colourArgb.has_value();
            colour = colourArgb.value_or (0);
        }
        infoDirty.store (true, std::memory_order_release);
    }

    wire::InfoRecord makeInfoRecord() const
    {
        wire::InfoRecord record;
        record.instanceId = id.load (std::memory_order_acquire);
        record.channels = numChannels.load (std::memory_order_relaxed) >= 2 ? 2 : 1;
        const std::lock_guard lock (infoMutex);
        record.name = trackName;
        record.hasColour = hasColour;
        record.colourArgb = hasColour ? colour : 0;
        return record;
    }

    /** The host-given track name ("" if none), for the editor. */
    wire::ShortText trackNameCopy() const
    {
        const std::lock_guard lock (infoMutex);
        return trackName;
    }

    std::optional<std::uint32_t> trackColour() const
    {
        const std::lock_guard lock (infoMutex);
        return hasColour ? std::optional<std::uint32_t> (colour) : std::nullopt;
    }

    // --- mailbox (bus writes, message thread drains) --------------------
    void postRemoteChange (const wire::ControlRecord& control)
    {
        const std::lock_guard lock (mailboxMutex);
        mailbox.mask |= control.mask;
        if ((control.mask & wire::controlMask::volume) != 0)
            mailbox.volume14 = control.volume14;
        if ((control.mask & wire::controlMask::pan) != 0)
            mailbox.pan = control.pan;
        if ((control.mask & wire::controlMask::mute) != 0)
            mailbox.muted = control.muted;
        mailbox.seq = control.seq;
    }

    RemoteChange takeRemoteChange()
    {
        const std::lock_guard lock (mailboxMutex);
        const auto change = mailbox;
        mailbox = {};
        return change;
    }

    // --- bus-private bookkeeping: touched ONLY under the registry lock ---
    struct BusPrivate
    {
        Clock::time_point registeredAt {};
        bool reported = false;
        std::uint64_t controlSender = 0;
        std::uint16_t controlSeq = 0;
        Clock::time_point leaseDeadline {};
        std::uint32_t lastBlocks = 0;
        Clock::time_point lastBlocksChange {};
        MeterHandoff::Reading lastReading {};
        Clock::time_point lastInfoSent {};
        wire::StateRecord lastSentState {};
        Clock::time_point lastStateSent {};
        bool hasSentState = false;
    };

    BusPrivate bus;

private:
    mutable std::mutex infoMutex;
    wire::ShortText trackName;
    bool hasColour = false;
    std::uint32_t colour = 0;

    std::mutex mailboxMutex;
    RemoteChange mailbox;
};

} // namespace mixersend
