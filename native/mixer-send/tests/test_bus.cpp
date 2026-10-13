// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "test_framework.h"

#include "bus/SendBus.h"
#include "core/FaderLaw.h"
#include "core/InstanceId.h"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #pragma comment (lib, "ws2_32.lib")
 using socklen_t = int;
 using TestSocket = SOCKET;
 static int pollOne (TestSocket s, int timeoutMs) { WSAPOLLFD p { s, POLLRDNORM, 0 }; return ::WSAPoll (&p, 1, timeoutMs); }
 static void closeTestSocket (TestSocket s) { ::closesocket (s); }
 static void startSockets() { static const bool started = [] { WSADATA d {}; return ::WSAStartup (MAKEWORD (2, 2), &d) == 0; }(); (void) started; }
#else
 #include <arpa/inet.h>
 #include <netinet/in.h>
 #include <poll.h>
 #include <sys/socket.h>
 #include <unistd.h>
 using TestSocket = int;
 static int pollOne (TestSocket s, int timeoutMs) { pollfd p { s, POLLIN, 0 }; return ::poll (&p, 1, timeoutMs); }
 static void closeTestSocket (TestSocket s) { ::close (s); }
 static void startSockets() {}
#endif

using namespace mixersend;
using namespace std::chrono_literals;

namespace
{
    /** A minimal companion: one UDP socket on 127.0.0.1:<ephemeral>. */
    class FakeCompanion
    {
    public:
        FakeCompanion()
        {
            startSockets();
            fd = ::socket (AF_INET, SOCK_DGRAM, 0);
            sockaddr_in a {};
           #if defined(__APPLE__)
            a.sin_len = sizeof (a);
           #endif
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
            ::bind (fd, reinterpret_cast<sockaddr*> (&a), sizeof (a));
            socklen_t len = sizeof (a);
            ::getsockname (fd, reinterpret_cast<sockaddr*> (&a), &len);
            port = ntohs (a.sin_port);
        }

        ~FakeCompanion() { closeTestSocket (fd); }

        /** Next valid datagram of `type` within `timeout`, remembering where it came from. */
        bool receive (wire::MessageType type, wire::Message& out, std::chrono::milliseconds timeout = 2000ms)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;

            while (std::chrono::steady_clock::now() < deadline)
            {
                if (pollOne (fd, 20) <= 0)
                    continue;

                std::uint8_t buffer[16384];
                sockaddr_in from {};
                socklen_t len = sizeof (from);
                const auto n = ::recvfrom (fd, reinterpret_cast<char*> (buffer), static_cast<int> (sizeof (buffer)), 0, reinterpret_cast<sockaddr*> (&from), &len);

                if (n <= 0 || wire::decode (buffer, static_cast<std::size_t> (n), out) != wire::DecodeError::none)
                    continue;

                peer = from;
                hasPeer = true;

                if (out.type == wire::MessageType::hello)
                    busId = out.senderId;

                if (out.type == type)
                    return true;
            }
            return false;
        }

        void send (const wire::Message& m)
        {
            std::vector<std::uint8_t> bytes;
            if (wire::encode (m, bytes) && hasPeer)
                ::sendto (fd, reinterpret_cast<const char*> (bytes.data()), static_cast<int> (bytes.size()), 0, reinterpret_cast<const sockaddr*> (&peer), sizeof (peer));
        }

        void sendRaw (const std::vector<std::uint8_t>& bytes)
        {
            if (hasPeer)
                ::sendto (fd, reinterpret_cast<const char*> (bytes.data()), static_cast<int> (bytes.size()), 0, reinterpret_cast<const sockaddr*> (&peer), sizeof (peer));
        }

        void welcome()
        {
            wire::Message m;
            m.type = wire::MessageType::welcome;
            m.senderId = session;
            m.targetBusId = busId;
            send (m);
        }

        /** hello -> welcome, then wait for the first state. */
        bool connect (wire::Message& firstState)
        {
            wire::Message hello;
            if (! receive (wire::MessageType::hello, hello))
                return false;
            welcome();
            return receive (wire::MessageType::state, firstState);
        }

        TestSocket fd {};
        std::uint16_t port = 0;
        std::uint64_t busId = 0;
        std::uint64_t session = 0xC0FFEEull;
        sockaddr_in peer {};
        bool hasPeer = false;
    };

    struct TestInstance
    {
        std::atomic<float> volume { 0.76f }, pan { 0.0f }, mute { 0.0f };
        InstanceLink link;
        std::atomic<int> remoteCallbacks { 0 };

        TestInstance()
        {
            link.id.store (randomNonZeroId());
            link.volumePosition = &volume;
            link.pan = &pan;
            link.mute = &mute;
            link.onRemoteChange = [this] { remoteCallbacks.fetch_add (1); };
        }
    };

    SendBus::Options fastOptions (std::uint16_t port)
    {
        SendBus::Options o;
        o.companionPort = port;
        o.connectedTickHz = 60;
        o.helloIntervalMs = 100;
        o.reportAfterMs = 0;
        return o;
    }

    SendBus::Identity testIdentity()
    {
        SendBus::Identity identity;
        identity.format = wire::Format::vst3;
        identity.hostName = "Test Host";
        identity.pluginVersion = 0x000100;
        return identity;
    }

    template <typename Predicate>
    bool waitFor (Predicate p, std::chrono::milliseconds timeout = 2000ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (p())
                return true;
            std::this_thread::sleep_for (5ms);
        }
        return p();
    }

    const wire::StateRecord* findState (const wire::Message& m, std::uint64_t id)
    {
        for (const auto& s : m.states)
            if (s.instanceId == id)
                return &s;
        return nullptr;
    }
}

TEST (bus_says_hello_reports_info_and_state_once_welcomed)
{
    FakeCompanion companion;
    SendBus bus (testIdentity(), fastOptions (companion.port));
    TestInstance a, b;
    a.link.setTrackProperties (std::string_view ("Kick"), 0xff3a7bd5u);
    a.volume.store (static_cast<float> (decibelsToFaderPosition (-6.0)));
    b.pan.store (-40.0f);
    b.mute.store (1.0f);
    bus.registerInstance (a.link);
    bus.registerInstance (b.link);

    // The bus says hello as soon as its thread starts -- possibly before both
    // registrations above -- so the count is a snapshot: wait for one that
    // reflects both (hellos repeat every 100 ms here).
    wire::Message hello;
    bool sawBoth = false;
    for (int i = 0; i < 20 && ! sawBoth; ++i)
        sawBoth = companion.receive (wire::MessageType::hello, hello) && hello.hello.instanceCount == 2;
    EXPECT (sawBoth);
    EXPECT (hello.senderId == bus.busId());
    EXPECT (hello.hello.format == wire::Format::vst3);
    EXPECT (std::string (hello.hello.hostName.view()) == "Test Host");
    EXPECT (! bus.isConnected());

    companion.welcome();

    wire::Message info;
    EXPECT (companion.receive (wire::MessageType::info, info));
    bool sawKick = false;
    for (const auto& r : info.info)
        if (r.instanceId == a.link.id.load())
            sawKick = std::string (r.name.view()) == "Kick" && r.hasColour && r.colourArgb == 0xff3a7bd5u;
    EXPECT (sawKick);

    wire::Message state;
    EXPECT (companion.receive (wire::MessageType::state, state));
    const auto* sa = findState (state, a.link.id.load());
    const auto* sb = findState (state, b.link.id.load());
    EXPECT (sa != nullptr && sb != nullptr);
    if (sa != nullptr && sb != nullptr)
    {
        EXPECT (sa->volume14 == faderPositionToVolume14 (decibelsToFaderPosition (-6.0)));
        EXPECT (sb->pan == -40);
        EXPECT ((sb->flags & wire::stateFlag::muted) != 0);
        EXPECT ((sa->flags & wire::stateFlag::processing) == 0); // no audio yet
    }

    EXPECT (waitFor ([&] { return bus.isConnected(); }));

    bus.unregisterInstance (a.link);
    bus.unregisterInstance (b.link);
}

TEST (controls_reach_the_instance_once_and_are_acknowledged)
{
    FakeCompanion companion;
    SendBus bus (testIdentity(), fastOptions (companion.port));
    TestInstance a;
    bus.registerInstance (a.link);

    wire::Message state;
    EXPECT (companion.connect (state));

    wire::Message control;
    control.type = wire::MessageType::control;
    control.senderId = companion.session;
    control.targetBusId = companion.busId;
    control.controls.push_back ({ a.link.id.load(), 1, wire::controlMask::volume | wire::controlMask::mute, 8000, 0, true });
    companion.send (control);

    EXPECT (waitFor ([&] { return a.remoteCallbacks.load() == 1; }));
    auto change = a.link.takeRemoteChange();
    EXPECT (change.mask == (wire::controlMask::volume | wire::controlMask::mute));
    EXPECT (change.volume14 == 8000 && change.muted && change.seq == 1);

    // A resend of the same seq is not taken twice.
    companion.send (control);
    std::this_thread::sleep_for (100ms);
    EXPECT (a.remoteCallbacks.load() == 1);

    // The processor applies it and acks; the ack travels back in the state.
    a.link.ackSeq.store (1);
    bool acked = false;
    for (int i = 0; i < 20 && ! acked; ++i)
    {
        wire::Message s;
        if (companion.receive (wire::MessageType::state, s))
            if (const auto* rec = findState (s, a.link.id.load()))
                acked = rec->ackSeq == 1;
    }
    EXPECT (acked);

    // A restarted companion (new session) starting again at seq 1 is obeyed.
    companion.session = 0xBEEFull;
    control.senderId = companion.session;
    control.controls[0].volume14 = 100;
    companion.send (control);
    EXPECT (waitFor ([&] { return a.remoteCallbacks.load() == 2; }));
    EXPECT (a.link.takeRemoteChange().volume14 == 100);

    // A control for another bus is ignored.
    control.targetBusId = companion.busId + 1;
    control.controls[0].seq = 2;
    companion.send (control);
    std::this_thread::sleep_for (100ms);
    EXPECT (a.remoteCallbacks.load() == 2);

    bus.unregisterInstance (a.link);
}

TEST (a_duplicated_instance_gets_a_fresh_id_in_process)
{
    FakeCompanion companion;
    SendBus bus (testIdentity(), fastOptions (companion.port));
    TestInstance original, copy, reloaded;
    bus.registerInstance (original.link);
    bus.registerInstance (copy.link);

    const auto originalId = original.link.id.load();
    const auto copyOwnId = copy.link.id.load();
    EXPECT (originalId != copyOwnId);

    // The host restores the original's state (id included) into the copy.
    EXPECT (bus.claimId (copy.link, originalId) == copyOwnId);
    EXPECT (copy.link.id.load() == copyOwnId);
    EXPECT (original.link.id.load() == originalId);

    // Reloading a project: the saved id is free, so it is adopted.
    const auto savedId = randomNonZeroId();
    bus.registerInstance (reloaded.link);
    EXPECT (bus.claimId (reloaded.link, savedId) == savedId);

    // A link registered with an id already live is re-rolled at registration.
    TestInstance clash;
    clash.link.id.store (originalId);
    bus.registerInstance (clash.link);
    EXPECT (clash.link.id.load() != originalId);

    // Once the original is gone, its id is free again (undo of a delete).
    bus.unregisterInstance (original.link);
    EXPECT (bus.claimId (copy.link, originalId) == originalId);

    bus.unregisterInstance (copy.link);
    bus.unregisterInstance (reloaded.link);
    bus.unregisterInstance (clash.link);
}

TEST (reassign_from_the_companion_rerolls_without_a_bye)
{
    FakeCompanion companion;
    SendBus bus (testIdentity(), fastOptions (companion.port));
    TestInstance a;
    bus.registerInstance (a.link);

    wire::Message state;
    EXPECT (companion.connect (state));
    const auto oldId = a.link.id.load();

    wire::Message reassign;
    reassign.type = wire::MessageType::reassign;
    reassign.senderId = companion.session;
    reassign.targetBusId = companion.busId;
    reassign.ids.push_back (oldId);
    companion.send (reassign);

    EXPECT (waitFor ([&] { return a.link.id.load() != oldId; }));

    wire::Message info;
    EXPECT (companion.receive (wire::MessageType::info, info));
    bool newIdReported = false;
    for (const auto& r : info.info)
        newIdReported |= r.instanceId == a.link.id.load();
    EXPECT (newIdReported);

    wire::Message bye;
    EXPECT (! companion.receive (wire::MessageType::bye, bye, 200ms));

    bus.unregisterInstance (a.link);
    EXPECT (companion.receive (wire::MessageType::bye, bye));
    EXPECT (bye.ids.size() == 1 && bye.ids[0] == a.link.id.load());
}

TEST (a_lease_mutes_and_lapses_on_its_own_when_the_companion_goes_quiet)
{
    FakeCompanion companion;
    SendBus bus (testIdentity(), fastOptions (companion.port));
    TestInstance a;
    bus.registerInstance (a.link);

    wire::Message state;
    EXPECT (companion.connect (state));

    wire::Message lease;
    lease.type = wire::MessageType::lease;
    lease.senderId = companion.session;
    lease.targetBusId = companion.busId;
    lease.leases.push_back ({ a.link.id.load(), true, 300 });
    companion.send (lease);

    EXPECT (waitFor ([&] { return a.link.leaseMuted.load(); }, 500ms));
    const auto mutedAt = std::chrono::steady_clock::now();

    // The companion says nothing more: the lease must end by itself.
    EXPECT (waitFor ([&] { return ! a.link.leaseMuted.load(); }, 1000ms));
    const auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - mutedAt).count();
    EXPECT_MSG (heldMs >= 200 && heldMs <= 450, std::to_string (heldMs) + " ms");

    // An absurd duration is capped at 5 s by the plugin. (Explicit release here.)
    lease.leases[0].durationMs = 60000;
    companion.send (lease);
    EXPECT (waitFor ([&] { return a.link.leaseMuted.load(); }, 500ms));
    lease.leases[0] = { a.link.id.load(), false, 0 };
    companion.send (lease);
    EXPECT (waitFor ([&] { return ! a.link.leaseMuted.load(); }, 500ms));

    bus.unregisterInstance (a.link);
}

TEST (garbage_is_counted_and_ignored)
{
    FakeCompanion companion;
    SendBus bus (testIdentity(), fastOptions (companion.port));
    TestInstance a;
    bus.registerInstance (a.link);

    wire::Message state;
    EXPECT (companion.connect (state));

    std::mt19937 rng (99);
    for (int i = 0; i < 2000; ++i)
    {
        std::vector<std::uint8_t> junk (rng() % 1500);
        for (auto& b : junk)
            b = static_cast<std::uint8_t> (rng());
        if (i % 3 == 0 && junk.size() >= 6)
        {
            junk[0] = 'M'; junk[1] = 'X'; junk[2] = 'S'; junk[3] = 'N'; junk[4] = 1; junk[5] = 0x11;
        }
        companion.sendRaw (junk);

        // Paced: an unpaced flood overruns the 64 KB socket buffer and the
        // kernel drops the excess -- correct, but then nothing is counted.
        if (i % 16 == 15)
            std::this_thread::sleep_for (1ms);
    }

    EXPECT (waitFor ([&] { return bus.stats().datagramsRejected >= 1900; }, 3000ms));

    // Still alive and obeying valid controls.
    wire::Message control;
    control.type = wire::MessageType::control;
    control.senderId = companion.session;
    control.targetBusId = companion.busId;
    control.controls.push_back ({ a.link.id.load(), 5, wire::controlMask::pan, 0, 33, false });
    companion.send (control);
    EXPECT (waitFor ([&] { return a.remoteCallbacks.load() == 1; }));
    EXPECT (a.link.takeRemoteChange().pan == 33);

    bus.unregisterInstance (a.link);
}

TEST (without_a_companion_the_bus_only_says_hello_and_drops_everything)
{
    std::uint16_t deadPort = 0;
    {
        FakeCompanion probe; // bind and release: nothing listens there now
        deadPort = probe.port;
    }

    SendBus::Options options = fastOptions (deadPort);
    options.helloIntervalMs = 50;
    SendBus bus (testIdentity(), options);
    TestInstance a;
    bus.registerInstance (a.link);

    std::this_thread::sleep_for (400ms);
    const auto stats = bus.stats();
    EXPECT (! bus.isConnected());
    EXPECT (stats.datagramsSent + stats.sendFailures <= 12); // hellos only, ~1 per 50 ms
    EXPECT (stats.ticks <= 12);

    bus.unregisterInstance (a.link);
}

TEST (a_connected_bus_wakes_once_per_tick_not_more)
{
    FakeCompanion companion;
    SendBus::Options options = fastOptions (companion.port);
    options.connectedTickHz = 30;
    options.helloIntervalMs = 1000;
    SendBus bus (testIdentity(), options);
    TestInstance a;
    bus.registerInstance (a.link);

    wire::Message state;
    EXPECT (companion.connect (state));

    std::atomic<bool> stop { false };
    std::thread keepWelcoming ([&] {
        wire::Message m;
        while (! stop.load())
            if (companion.receive (wire::MessageType::hello, m, 200ms))
                companion.welcome();
    });

    const auto before = bus.stats();
    std::this_thread::sleep_for (3000ms);
    const auto after = bus.stats();
    stop.store (true);
    keepWelcoming.join();

    const auto wakeups = after.wakeups - before.wakeups;
    const auto ticks = after.ticks - before.ticks;
    std::printf ("      connected and idle: %llu wakeups for %llu ticks in 3 s\n",
                 static_cast<unsigned long long> (wakeups), static_cast<unsigned long long> (ticks));
    EXPECT (bus.isConnected());
    EXPECT (ticks >= 70 && ticks <= 95); // 30 Hz; wall-clock, so a little slack for a loaded machine
    EXPECT (wakeups <= ticks + 20); // + the welcome datagrams

    bus.unregisterInstance (a.link);
}

TEST (a_companion_that_dies_costs_the_bus_almost_nothing)
{
    auto companion = std::make_unique<FakeCompanion>();
    SendBus::Options options = fastOptions (companion->port);
    options.connectedTickHz = 30;
    options.helloIntervalMs = 1000;
    SendBus bus (testIdentity(), options);

    std::vector<std::unique_ptr<TestInstance>> instances;
    for (int i = 0; i < 16; ++i)
    {
        instances.push_back (std::make_unique<TestInstance>());
        bus.registerInstance (instances.back()->link);
    }

    wire::Message state;
    EXPECT (companion->connect (state));
    EXPECT (waitFor ([&] { return bus.isConnected(); }));

    companion.reset(); // the companion process is gone: its port is closed

    const auto before = bus.stats();
    std::this_thread::sleep_for (4500ms);
    const auto after = bus.stats();

    const auto wakeups = after.wakeups - before.wakeups;
    std::printf ("      after the companion died: %llu wakeups, %llu ticks, %llu sends, %llu failed sends in 4.5 s\n",
                 static_cast<unsigned long long> (wakeups), static_cast<unsigned long long> (after.ticks - before.ticks),
                 static_cast<unsigned long long> (after.datagramsSent - before.datagramsSent),
                 static_cast<unsigned long long> (after.sendFailures - before.sendFailures));

    // ~3 s of 30 Hz ticks until the link times out, then 1 Hz hellos. A
    // spinning loop would show tens of thousands.
    EXPECT (wakeups < 300);
    EXPECT (! bus.isConnected());

    for (auto& inst : instances)
        bus.unregisterInstance (inst->link);
}

TEST (register_and_unregister_churn_while_the_bus_runs_is_safe)
{
    FakeCompanion companion;
    SendBus::Options options = fastOptions (companion.port);
    options.connectedTickHz = 120;
    SendBus bus (testIdentity(), options);

    TestInstance steady;
    bus.registerInstance (steady.link);
    wire::Message state;
    EXPECT (companion.connect (state));

    std::atomic<bool> stop { false };

    // The companion floods controls and leases at the steady instance and at
    // ids that come and go, while two threads churn instances.
    std::thread flooder ([&] {
        std::uint16_t seq = 1;
        while (! stop.load())
        {
            wire::Message control;
            control.type = wire::MessageType::control;
            control.senderId = companion.session;
            control.targetBusId = companion.busId;
            control.controls.push_back ({ steady.link.id.load(), seq++, wire::controlMask::volume, static_cast<std::uint16_t> (seq % 16000), 0, false });
            companion.send (control);
            if (seq == 0)
                seq = 1;
            companion.welcome();
            std::this_thread::sleep_for (1ms);
        }
    });

    auto churn = [&] (unsigned seed) {
        std::mt19937 rng (seed);
        for (int i = 0; i < 400; ++i)
        {
            auto instance = std::make_unique<TestInstance>();
            bus.registerInstance (instance->link);
            instance->link.setTrackProperties (std::string_view ("churn"), std::nullopt);
            bus.wake();
            std::this_thread::sleep_for (std::chrono::microseconds (rng() % 500));
            bus.unregisterInstance (instance->link);
            // instance (and its atomics) destroyed right here, while the bus runs
        }
    };

    std::thread c1 (churn, 1u), c2 (churn, 2u);
    c1.join();
    c2.join();
    stop.store (true);
    flooder.join();

    EXPECT (bus.instanceCount() == 1);
    EXPECT (steady.remoteCallbacks.load() > 0);
    bus.unregisterInstance (steady.link);
}

TEST (bench_bus_cost_with_128_instances_and_live_meters)
{
    FakeCompanion companion;
    SendBus bus (testIdentity(), [&] { auto o = fastOptions (companion.port); o.connectedTickHz = 30; return o; }());

    std::vector<std::unique_ptr<TestInstance>> instances;
    for (int i = 0; i < 128; ++i)
    {
        instances.push_back (std::make_unique<TestInstance>());
        bus.registerInstance (instances.back()->link);
    }

    wire::Message state;
    EXPECT (companion.connect (state));

    // A fake audio thread publishing changing levels every 1.33 ms.
    std::atomic<bool> stop { false };
    std::thread audio ([&] {
        std::mt19937 rng (5);
        std::uniform_real_distribution<float> level (0.01f, 0.9f);
        while (! stop.load())
        {
            for (auto& inst : instances)
            {
                BlockLevels levels;
                levels.peak[0] = level (rng);
                levels.peak[1] = level (rng);
                levels.sumSquares[0] = levels.peak[0] * levels.peak[0] * 20.0f;
                levels.sumSquares[1] = levels.peak[1] * levels.peak[1] * 20.0f;
                inst->link.meters.push (levels, 2, 64);
            }
            std::this_thread::sleep_for (std::chrono::microseconds (1333));
        }
    });

    // The companion keeps reading (so the socket never fills).
    std::atomic<int> datagrams { 0 };
    std::thread reader ([&] {
        wire::Message m;
        while (! stop.load())
            if (companion.receive (wire::MessageType::state, m, 100ms))
                datagrams.fetch_add (1);
    });

    const auto before = bus.stats();
    std::this_thread::sleep_for (3000ms);
    const auto after = bus.stats();
    stop.store (true);
    audio.join();
    reader.join();

    const auto ticks = after.ticks - before.ticks;
    const double perTickUs = ticks > 0 ? (after.tickNanos - before.tickNanos) / 1000.0 / static_cast<double> (ticks) : 0.0;
    std::printf ("      128 instances, live meters: %llu ticks in 3 s, %.1f us per tick (%.3f %% of one core), %d state datagrams\n",
                 static_cast<unsigned long long> (ticks), perTickUs, perTickUs * static_cast<double> (ticks) / 3.0e6 * 100.0,
                 datagrams.load());
    EXPECT (ticks >= 80);
    EXPECT (perTickUs < 1000.0);

    for (auto& inst : instances)
        bus.unregisterInstance (inst->link);
}

TEST (the_process_wide_bus_is_shared_and_torn_down_with_its_last_user)
{
    const auto identity = testIdentity();
    std::uint16_t deadPort = 0;
    {
        FakeCompanion probe;
        deadPort = probe.port;
    }
    // acquire() reads the port from the environment, like the plugin does;
    // keep this test away from a real companion on 4891.
   #if defined(_WIN32)
    ::_putenv_s ("MIXER_SEND_COMPANION_PORT", std::to_string (deadPort).c_str());
   #else
    ::setenv ("MIXER_SEND_COMPANION_PORT", std::to_string (deadPort).c_str(), 1);
   #endif

    std::weak_ptr<SendBus> weak;
    {
        auto first = SendBus::acquire (identity);
        auto second = SendBus::acquire (identity);
        EXPECT (first == second);
        EXPECT (first->isRunning());
        weak = first;
    }
    EXPECT (weak.expired());

    auto fresh = SendBus::acquire (identity);
    EXPECT (fresh != nullptr && fresh->isRunning());
}
