// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "bus/InstanceLink.h"
#include "bus/UdpEndpoint.h"
#include "core/WireProtocol.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mixersend
{

/*  The ONE connection to the companion for every Mixer SEND instance that
    this loaded plugin binary has in this process.

    One socket and one low-priority thread no matter how many instances: at
    ~30 Hz it batches every live instance's state into a few datagrams, and
    it dispatches the companion's controls to instances by id. A host that
    runs plugins in several processes (Bitwig's sandbox, Logic's AU hosting
    service) simply gets one bus per process; the companion tells them apart
    by bus id, so nothing here assumes a single process.

    Real-time contract: the audio thread never calls into the bus. Message
    thread calls (register, unregister, claimId, wake) take the registry lock,
    which the bus thread only ever holds for a short copy, never across a
    system call it could block in. Sends are non-blocking and dropped on
    failure; with the companion absent the bus sends one hello per second and
    keeps nothing. */
class SendBus
{
public:
    using Clock = std::chrono::steady_clock;

    struct Identity
    {
        wire::Format format = wire::Format::unknown;
        std::string hostName;
        std::uint32_t pluginVersion = 0;
    };

    struct Options
    {
        std::uint16_t companionPort = wire::kDefaultCompanionPort;
        int connectedTickHz = 30;
        int helloIntervalMs = 1000;
        int infoRefreshMs = 5000;
        int connectionTimeoutMs = 3000;
        /** Instances younger than this are not reported: a host's plugin scan
            creates and destroys instances within milliseconds, and those must
            not flash up on the surface (or in its saved display order). */
        int reportAfterMs = 1000;
        int maxLeaseMs = 5000;
        /** Meters hold their last reading this long when no audio block
            arrived (large host buffers), then read as silence. */
        int meterHoldMs = 250;
        int processingTimeoutMs = 500;
        /** An unchanged instance is re-sent at least this often, so the
            companion's 2 s staleness never fires for a live one. Changed
            state goes out on the next tick. */
        int stateKeepaliveMs = 250;

        /** Defaults, with MIXER_SEND_COMPANION_PORT honoured (tests only). */
        static Options fromEnvironment();
    };

    struct Stats
    {
        std::uint64_t datagramsSent = 0;
        std::uint64_t sendFailures = 0;
        std::uint64_t datagramsReceived = 0;
        std::uint64_t datagramsRejected = 0;
        std::uint64_t controlsApplied = 0;
        std::uint64_t ticks = 0;
        std::uint64_t tickNanos = 0; ///< wall time spent inside tick(), summed
        std::uint64_t wakeups = 0;   ///< loop iterations (poll returns)
    };

    /** The process-wide bus for this binary, created on first use and
        destroyed (thread joined, socket closed) with its last user. */
    static std::shared_ptr<SendBus> acquire (const Identity& identity);

    /** A standalone bus (tests). Production code uses acquire(). */
    SendBus (const Identity& identity, const Options& options);
    ~SendBus();

    SendBus (const SendBus&) = delete;
    SendBus& operator= (const SendBus&) = delete;

    /** Registers a link whose `id`, parameter pointers and callback are set.
        An id already live in this process is replaced by a fresh one. */
    void registerInstance (InstanceLink& link);

    /** After this returns the bus never touches `link` again. */
    void unregisterInstance (InstanceLink& link);

    /** Adopts `requested` (from saved state) as the link's id unless another
        live instance in this process already holds it -- a duplicated
        instance -- in which case the link keeps its own fresh id. Returns the
        id the link ends up with. */
    std::uint64_t claimId (InstanceLink& link, std::uint64_t requested);

    /** Nudges the thread (e.g. after a name change) -- any thread. */
    void wake() noexcept { endpoint.wake(); }

    bool isConnected() const noexcept { return connected.load (std::memory_order_relaxed); }
    std::uint64_t busId() const noexcept { return id; }
    std::uint16_t localPort() const noexcept { return endpoint.localPort(); }
    bool isRunning() const noexcept { return thread.joinable(); }
    std::size_t instanceCount() const;
    Stats stats() const noexcept;

private:
    void run();
    void drainIncoming (Clock::time_point now);
    void handleMessage (const wire::Message& message, Clock::time_point now);
    void tick (Clock::time_point now);
    void sendHello (std::size_t instances);
    void sendFinalBye();
    void flush (wire::DatagramBuilder& builder);
    Clock::time_point nextWakeDeadline (Clock::time_point now);
    void expireLeases (Clock::time_point now);
    InstanceLink* findLocked (std::uint64_t instanceId) const noexcept;
    std::uint64_t uniqueIdLocked() const noexcept;

    const std::uint64_t id;
    const Identity identity;
    const Options options;

    UdpEndpoint endpoint;
    std::thread thread;
    std::atomic<bool> stopRequested { false };
    std::atomic<bool> connected { false };

    mutable std::mutex registryMutex;
    std::vector<InstanceLink*> links;          // guarded by registryMutex
    std::vector<std::uint64_t> pendingByes;    // guarded by registryMutex

    // --- bus thread only ------------------------------------------------
    Clock::time_point lastCompanionPacket {};
    bool everHeardCompanion = false;
    bool wasConnected = false;
    bool forceFullInfo = true;
    bool forceFullState = true;
    std::uint64_t companionSession = 0;
    Clock::time_point nextHello {};
    Clock::time_point nextTick {};
    Clock::time_point nextFullInfo {};
    wire::Message incoming;
    wire::DatagramBuilder builder;
    std::vector<wire::StateRecord> stateScratch;
    std::vector<wire::InfoRecord> infoScratch;
    std::vector<std::uint64_t> byeScratch;
    std::uint8_t receiveBuffer[2048] {};

    std::atomic<std::uint64_t> sent { 0 }, received { 0 }, rejected { 0 }, applied { 0 }, ticks { 0 }, tickNanos { 0 }, wakeups { 0 };
};

} // namespace mixersend
