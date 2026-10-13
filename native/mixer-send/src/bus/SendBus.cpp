// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "bus/SendBus.h"

#include "core/FaderLaw.h"
#include "core/InstanceId.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <unistd.h>
#endif

#if defined(__APPLE__)
 #include <pthread.h>
 #include <pthread/qos.h>
#endif

namespace mixersend
{

namespace
{
    using Clock = SendBus::Clock;
    using std::chrono::milliseconds;

    /** Deliberately leaked: plugin instances can be destroyed during process
        teardown, after function-local statics would have been destroyed. */
    struct Global
    {
        std::mutex mutex;
        std::weak_ptr<SendBus> bus;
    };

    Global& global()
    {
        static auto* g = new Global();
        return *g;
    }

    void configureBusThread()
    {
       #if defined(__APPLE__)
        pthread_setname_np ("Mixer SEND bus");
        // Utility, not background: background QoS may be parked for seconds
        // under load, which would read as a dead link. Either way it is far
        // below the audio threads.
        pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
       #elif defined(_WIN32)
        // Named for debuggers and profilers. SetThreadDescription exists from
        // Windows 10 1607 on; looked up at run time so the DLL still loads
        // (without a thread name) where it does not. Normal priority: far
        // below the host's audio threads, never parked for seconds.
        using SetDescription = HRESULT (WINAPI*) (HANDLE, PCWSTR);
        if (auto* kernel = ::GetModuleHandleW (L"kernel32.dll"))
            if (auto* set = reinterpret_cast<SetDescription> (reinterpret_cast<void*> (::GetProcAddress (kernel, "SetThreadDescription"))))
                set (::GetCurrentThread(), L"Mixer SEND bus");
       #endif
    }

    std::uint32_t currentProcessId() noexcept
    {
       #if defined(_WIN32)
        return static_cast<std::uint32_t> (::GetCurrentProcessId());
       #else
        return static_cast<std::uint32_t> (::getpid());
       #endif
    }

    std::uint16_t portFromEnvironment()
    {
        if (const char* text = std::getenv ("MIXER_SEND_COMPANION_PORT"))
        {
            char* end = nullptr;
            const long value = std::strtol (text, &end, 10);

            if (end != text && *end == '\0' && value > 0 && value < 65536)
                return static_cast<std::uint16_t> (value);
        }
        return wire::kDefaultCompanionPort;
    }

    int msUntil (Clock::time_point from, Clock::time_point to)
    {
        if (to <= from)
            return 0;
        return static_cast<int> (std::chrono::duration_cast<milliseconds> (to - from).count()) + 1;
    }
}

//==============================================================================
SendBus::Options SendBus::Options::fromEnvironment()
{
    Options options;
    options.companionPort = portFromEnvironment();
    return options;
}

std::shared_ptr<SendBus> SendBus::acquire (const Identity& identity)
{
    auto& g = global();
    const std::lock_guard lock (g.mutex);

    if (auto existing = g.bus.lock())
        return existing;

    auto bus = std::make_shared<SendBus> (identity, Options::fromEnvironment());
    g.bus = bus;
    return bus;
}

SendBus::SendBus (const Identity& identityToUse, const Options& optionsToUse)
    : id (randomNonZeroId()), identity (identityToUse), options (optionsToUse)
{
    builder.setCapacity (wire::kMaxPluginDatagramBytes);
    links.reserve (256);
    stateScratch.reserve (256);
    infoScratch.reserve (256);
    byeScratch.reserve (64);

    // Without a socket the instances still work as plain gain/pan plugins;
    // there is just nothing for a thread to do.
    if (! endpoint.open (options.companionPort))
    {
        endpoint.close();
        return;
    }

    try
    {
        thread = std::thread ([this] { run(); });
    }
    catch (...)
    {
        endpoint.close();
    }
}

SendBus::~SendBus()
{
    stopRequested.store (true, std::memory_order_release);
    endpoint.wake();

    if (thread.joinable())
        thread.join();

    endpoint.close();
}

std::size_t SendBus::instanceCount() const
{
    const std::lock_guard lock (registryMutex);
    return links.size();
}

SendBus::Stats SendBus::stats() const noexcept
{
    Stats s;
    s.datagramsSent = sent.load();
    s.sendFailures = endpoint.sendFailures();
    s.datagramsReceived = received.load();
    s.datagramsRejected = rejected.load();
    s.controlsApplied = applied.load();
    s.ticks = ticks.load();
    s.tickNanos = tickNanos.load();
    s.wakeups = wakeups.load();
    return s;
}

//==============================================================================
InstanceLink* SendBus::findLocked (std::uint64_t instanceId) const noexcept
{
    for (auto* link : links)
        if (link->id.load (std::memory_order_relaxed) == instanceId)
            return link;
    return nullptr;
}

std::uint64_t SendBus::uniqueIdLocked() const noexcept
{
    for (;;)
    {
        const auto candidate = randomNonZeroId();
        if (findLocked (candidate) == nullptr)
            return candidate;
    }
}

void SendBus::registerInstance (InstanceLink& link)
{
    {
        const std::lock_guard lock (registryMutex);

        if (std::find (links.begin(), links.end(), &link) != links.end())
            return;

        const auto current = link.id.load();
        if (current == 0 || findLocked (current) != nullptr)
            link.id.store (uniqueIdLocked());

        link.bus = {};
        link.bus.registeredAt = Clock::now();
        link.infoDirty.store (true);
        links.push_back (&link);
    }

    endpoint.wake();
}

void SendBus::unregisterInstance (InstanceLink& link)
{
    {
        const std::lock_guard lock (registryMutex);
        const auto it = std::find (links.begin(), links.end(), &link);

        if (it == links.end())
            return;

        if (link.bus.reported)
            pendingByes.push_back (link.id.load());

        links.erase (it);

        // Nothing may be queued forever: without a companion to hear them,
        // byes are dropped at the next tick. The cap only guards a host that
        // churns thousands of instances between two ticks.
        if (pendingByes.size() > 4096)
            pendingByes.erase (pendingByes.begin(), pendingByes.begin() + 2048);
    }

    // A lease must never outlive its instance in some other form: the link is
    // gone, so its leaseMuted flag dies with the processor.
    endpoint.wake();
}

std::uint64_t SendBus::claimId (InstanceLink& link, std::uint64_t requested)
{
    const std::lock_guard lock (registryMutex);
    const auto current = link.id.load();

    if (requested == 0 || requested == current)
        return current;

    const bool registered = std::find (links.begin(), links.end(), &link) != links.end();

    for (auto* other : links)
        if (other != &link && other->id.load() == requested)
            return current; // duplicated instance: keep our own fresh id

    if (registered && link.bus.reported)
        pendingByes.push_back (current); // that id was ours alone, and it is going away

    link.id.store (requested);
    link.infoDirty.store (true);
    return requested;
}

//==============================================================================
void SendBus::run()
{
    configureBusThread();

    const auto start = Clock::now();
    nextHello = start;
    nextTick = start;
    nextFullInfo = start;

    while (! stopRequested.load (std::memory_order_acquire))
    {
        const auto now = Clock::now();
        const auto deadline = nextWakeDeadline (now);
        endpoint.wait (std::min (msUntil (now, deadline), 1000));
        wakeups.fetch_add (1, std::memory_order_relaxed);

        if (stopRequested.load (std::memory_order_acquire))
            break;

        drainIncoming (Clock::now());

        const auto after = Clock::now();

        // Every deadline the wait above can wake for is handled right here,
        // so a wake-up always makes progress and the loop can never spin.
        expireLeases (after);

        if (after >= nextTick)
        {
            tick (after);
            tickNanos.fetch_add (static_cast<std::uint64_t> (
                std::chrono::duration_cast<std::chrono::nanoseconds> (Clock::now() - after).count()), std::memory_order_relaxed);

            const auto period = connected.load()
                                  ? std::chrono::duration_cast<Clock::duration> (std::chrono::duration<double> (1.0 / std::max (1, options.connectedTickHz)))
                                  : std::chrono::duration_cast<Clock::duration> (milliseconds (options.helloIntervalMs));

            // Scheduled from the previous deadline, not from when this tick
            // happened to run, so a late wake-up does not slow the rate down;
            // after a long stall it restarts from now instead of bursting.
            nextTick += period;
            if (nextTick <= after)
                nextTick = after + period;
        }
    }

    sendFinalBye();
}

Clock::time_point SendBus::nextWakeDeadline (Clock::time_point now)
{
    // Only deadlines the loop acts on as soon as it wakes: the tick (hellos
    // ride on ticks) and active leases (expireLeases runs every iteration).
    // Waking for anything else would return straight to an immediate poll.
    auto deadline = nextTick;

    // An active lease must lapse on time even while ticks are slow.
    const std::lock_guard lock (registryMutex);
    for (auto* link : links)
        if (link->leaseMuted.load (std::memory_order_relaxed))
            deadline = std::min (deadline, link->bus.leaseDeadline);

    return std::max (deadline, now);
}

void SendBus::expireLeases (Clock::time_point now)
{
    const std::lock_guard lock (registryMutex);
    for (auto* link : links)
        if (link->leaseMuted.load (std::memory_order_relaxed) && now >= link->bus.leaseDeadline)
            link->leaseMuted.store (false, std::memory_order_release);
}

void SendBus::drainIncoming (Clock::time_point now)
{
    // Bounded, so a flood of datagrams can delay a tick but never starve it.
    for (int i = 0; i < 256; ++i)
    {
        const long size = endpoint.receive (receiveBuffer, sizeof (receiveBuffer));

        if (size < 0)
            return;

        received.fetch_add (1, std::memory_order_relaxed);

        if (wire::decode (receiveBuffer, static_cast<std::size_t> (size), incoming) != wire::DecodeError::none)
        {
            rejected.fetch_add (1, std::memory_order_relaxed);
            continue;
        }

        handleMessage (incoming, now);
    }
}

void SendBus::handleMessage (const wire::Message& message, Clock::time_point now)
{
    switch (message.type)
    {
        case wire::MessageType::welcome:
        case wire::MessageType::control:
        case wire::MessageType::lease:
        case wire::MessageType::reassign:
            break;

        case wire::MessageType::hello:
        case wire::MessageType::info:
        case wire::MessageType::state:
        case wire::MessageType::bye:
            rejected.fetch_add (1, std::memory_order_relaxed); // plugin -> companion types
            return;
    }

    if (message.targetBusId != id)
    {
        rejected.fetch_add (1, std::memory_order_relaxed); // meant for a previous bus on this port
        return;
    }

    lastCompanionPacket = now;
    everHeardCompanion = true;

    if (message.senderId != companionSession)
    {
        // A new companion (or a restarted one): it knows nothing yet.
        companionSession = message.senderId;
        forceFullInfo = true;
        forceFullState = true;
    }

    if (! connected.load (std::memory_order_relaxed))
    {
        connected.store (true, std::memory_order_relaxed);
        forceFullInfo = true;
        forceFullState = true;
        nextTick = now; // start reporting right away
    }

    const std::lock_guard lock (registryMutex);

    switch (message.type)
    {
        case wire::MessageType::control:
            for (const auto& control : message.controls)
            {
                auto* link = findLocked (control.instanceId);

                if (link == nullptr)
                    continue;

                auto& b = link->bus;
                const bool fromNewSession = b.controlSender != message.senderId;

                if (! fromNewSession && ! wire::isNewerSeq (control.seq, b.controlSeq))
                    continue; // a resend of something already taken

                b.controlSender = message.senderId;
                b.controlSeq = control.seq;
                link->postRemoteChange (control);
                applied.fetch_add (1, std::memory_order_relaxed);

                if (link->onRemoteChange)
                    link->onRemoteChange();
            }
            break;

        case wire::MessageType::lease:
            for (const auto& lease : message.leases)
            {
                auto* link = findLocked (lease.instanceId);

                if (link == nullptr)
                    continue;

                if (lease.mute && lease.durationMs > 0)
                {
                    const int duration = std::min<int> (lease.durationMs, options.maxLeaseMs);
                    link->bus.leaseDeadline = now + milliseconds (duration);
                    link->leaseMuted.store (true, std::memory_order_release);
                }
                else
                {
                    link->leaseMuted.store (false, std::memory_order_release);
                }
            }
            break;

        case wire::MessageType::reassign:
            for (const auto instanceId : message.ids)
            {
                if (auto* link = findLocked (instanceId))
                {
                    // No bye for the old id: the companion asked because some
                    // OTHER bus owns it, and a bye would remove that instance.
                    link->id.store (uniqueIdLocked());
                    link->infoDirty.store (true);
                }
            }
            forceFullInfo = true;
            break;

        case wire::MessageType::welcome:
        case wire::MessageType::hello:
        case wire::MessageType::info:
        case wire::MessageType::state:
        case wire::MessageType::bye:
            break;
    }
}

//==============================================================================
void SendBus::flush (wire::DatagramBuilder& b)
{
    if (b.count() == 0)
        return;

    if (endpoint.send (b.data(), b.size()))
    {
        sent.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // A system with a lower UDP size limit than stock macOS: fall back to
    // small datagrams for good (this tick's batch is simply dropped).
    if (endpoint.lastSendWasTooLarge())
        b.setCapacity (wire::kMaxDatagramBytes);
}

void SendBus::sendHello (std::size_t instances)
{
    wire::Hello hello;
    hello.pid = currentProcessId();
    hello.pluginVersion = identity.pluginVersion;
    hello.format = identity.format;
    hello.tickHz = static_cast<std::uint8_t> (std::clamp (options.connectedTickHz, 1, 120));
    hello.instanceCount = static_cast<std::uint16_t> (std::min<std::size_t> (instances, 0xFFFF));
    hello.hostName.assignSanitised (identity.hostName);

    builder.begin (wire::MessageType::hello, id);
    if (builder.setHello (hello) && endpoint.send (builder.data(), builder.size()))
        sent.fetch_add (1, std::memory_order_relaxed);
}

void SendBus::tick (Clock::time_point now)
{
    ticks.fetch_add (1, std::memory_order_relaxed);

    const bool connectedNow = everHeardCompanion
                           && now - lastCompanionPacket < milliseconds (options.connectionTimeoutMs);

    if (connectedNow && ! wasConnected)
    {
        forceFullInfo = true;
        forceFullState = true;
    }

    wasConnected = connectedNow;
    connected.store (connectedNow, std::memory_order_relaxed);

    const bool fullInfo = forceFullInfo || now >= nextFullInfo;
    const auto reportAfter = milliseconds (options.reportAfterMs);
    const auto holdFor = milliseconds (options.meterHoldMs);
    const auto processingFor = milliseconds (options.processingTimeoutMs);
    const auto keepalive = milliseconds (options.stateKeepaliveMs);
    const bool fullState = forceFullState;

    stateScratch.clear();
    infoScratch.clear();
    byeScratch.clear();
    std::size_t instances = 0;

    {
        const std::lock_guard lock (registryMutex);
        instances = links.size();
        byeScratch.swap (pendingByes);

        for (auto* link : links)
        {
            auto& b = link->bus;

            // Leases lapse here whether or not a companion is listening.
            if (link->leaseMuted.load (std::memory_order_relaxed) && now >= b.leaseDeadline)
                link->leaseMuted.store (false, std::memory_order_release);

            if (! connectedNow || now - b.registeredAt < reportAfter)
                continue;

            // --- meters: hold briefly across a gap between host blocks ----
            const auto reading = link->meters.take();

            if (reading.blocks != b.lastBlocks)
            {
                b.lastBlocks = reading.blocks;
                b.lastBlocksChange = now;
                b.lastReading = reading;
            }

            const auto sinceAudio = now - b.lastBlocksChange;
            const bool hasAudio = b.lastBlocksChange != Clock::time_point {} && sinceAudio < holdFor;

            wire::StateRecord record;
            record.instanceId = link->id.load (std::memory_order_relaxed);
            record.volume14 = static_cast<std::uint16_t> (faderPositionToVolume14 (
                link->volumePosition != nullptr ? link->volumePosition->load (std::memory_order_relaxed) : kUnityPosition));

            const float panValue = link->pan != nullptr ? link->pan->load (std::memory_order_relaxed) : 0.0f;
            record.pan = static_cast<std::int8_t> (std::clamp<long> (std::lround (std::isfinite (panValue) ? panValue : 0.0f), -100, 100));

            const bool muted = link->mute != nullptr && link->mute->load (std::memory_order_relaxed) > 0.5f;
            record.flags = static_cast<std::uint8_t> (
                  (muted ? wire::stateFlag::muted : 0)
                | (link->leaseMuted.load (std::memory_order_relaxed) ? wire::stateFlag::leaseMuted : 0)
                | (b.lastBlocksChange != Clock::time_point {} && sinceAudio < processingFor ? wire::stateFlag::processing : 0)
                | (link->bypassed.load (std::memory_order_relaxed) ? wire::stateFlag::bypassed : 0));

            if (hasAudio)
            {
                record.peakL = wire::meterCodeFromAmplitude (b.lastReading.peak[0]);
                record.peakR = wire::meterCodeFromAmplitude (b.lastReading.peak[1]);
                record.rmsL = wire::meterCodeFromAmplitude (b.lastReading.rms[0]);
                record.rmsR = wire::meterCodeFromAmplitude (b.lastReading.rms[1]);
            }

            record.ackSeq = link->ackSeq.load (std::memory_order_acquire);
            b.reported = true;

            // Only what changed, plus a keepalive: an idle project costs a
            // few datagrams a second instead of thirty.
            const auto& last = b.lastSentState;
            const bool changed = ! b.hasSentState || record.instanceId != last.instanceId
                              || record.volume14 != last.volume14 || record.pan != last.pan
                              || record.flags != last.flags || record.peakL != last.peakL
                              || record.peakR != last.peakR || record.rmsL != last.rmsL
                              || record.rmsR != last.rmsR || record.ackSeq != last.ackSeq;

            if (fullState || changed || now - b.lastStateSent >= keepalive)
            {
                stateScratch.push_back (record);
                b.lastSentState = record;
                b.lastStateSent = now;
                b.hasSentState = true;
            }

            if (fullInfo || link->infoDirty.exchange (false))
            {
                infoScratch.push_back (link->makeInfoRecord());
                b.lastInfoSent = now;
            }
        }
    }

    if (now >= nextHello)
    {
        sendHello (instances);
        nextHello = now + milliseconds (options.helloIntervalMs);
    }

    if (! connectedNow)
        return; // byes, info and state are only for a companion that is there

    if (fullInfo)
    {
        forceFullInfo = false;
        nextFullInfo = now + milliseconds (options.infoRefreshMs);
    }

    forceFullState = false;

    if (! byeScratch.empty())
    {
        builder.begin (wire::MessageType::bye, id);
        for (const auto instanceId : byeScratch)
        {
            if (! builder.addId (instanceId))
            {
                flush (builder);
                builder.begin (wire::MessageType::bye, id);
                builder.addId (instanceId);
            }
        }
        flush (builder);
    }

    if (! infoScratch.empty())
    {
        builder.begin (wire::MessageType::info, id);
        for (const auto& record : infoScratch)
        {
            if (! builder.add (record))
            {
                flush (builder);
                builder.begin (wire::MessageType::info, id);
                builder.add (record);
            }
        }
        flush (builder);
    }

    if (! stateScratch.empty())
    {
        builder.begin (wire::MessageType::state, id);
        for (const auto& record : stateScratch)
        {
            if (! builder.add (record))
            {
                flush (builder);
                builder.begin (wire::MessageType::state, id);
                builder.add (record);
            }
        }
        flush (builder);
    }
}

void SendBus::sendFinalBye()
{
    // Best effort, so the surface drops these channels at once instead of
    // after the staleness timeout. Nothing waits for it.
    if (! connected.load())
        return;

    builder.begin (wire::MessageType::bye, id); // count 0: the whole bus
    if (endpoint.send (builder.data(), builder.size()))
        sent.fetch_add (1, std::memory_order_relaxed);
}

} // namespace mixersend
