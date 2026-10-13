// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

/*
    Mixer SEND host harness.

    Loads the BUILT plugin (VST3 bundle, or the installed AU) through JUCE's
    plugin-format classes -- the same path a DAW takes -- and runs many
    instances on a real-time audio thread while a fake companion, speaking the
    real wire protocol over real UDP, drives them through a scripted timeline:

      connect -> remote volume / pan / mute -> solo lease that must lapse ->
      garbage flood -> companion killed -> companion restarted (new session) ->
      instance add/remove churn and a duplicated instance -> steady state.

    It reports the audio thread's cost per block and per instance, every heap
    allocation made on the audio thread while inside the plugins
    (libmalloc's malloc_logger hook, all binaries included), the bus thread's
    CPU time, and a PASS/FAIL line per functional check.

    usage:
      mixer_send_harness --plugin <path.vst3 | path.component> [--instances 128]
                         [--seconds 30] [--block 64] [--rate 48000]
                         [--no-companion] [--fast]
      mixer_send_harness --plugin <path> --e2e-http http://127.0.0.1:<port> --pairing-code <6 digits>
                         --udp-port <companion udp port>
*/

#include "core/FaderLaw.h"
#include "core/InstanceId.h"
#include "core/WireProtocol.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <arpa/inet.h>
#include <execinfo.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <thread>

using namespace juce;
using namespace mixersend;

//==============================================================================
// Heap allocations made by the audio thread while it is inside the plugins.
// malloc_logger is libmalloc's own hook (used by the stack-logging tools): it
// sees every malloc/calloc/realloc/new from every image in the process. The
// hook must not touch thread_local storage (that can allocate), so the thread
// is compared by pthread_self().
//==============================================================================
typedef void (malloc_logger_t) (uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t);
extern "C" malloc_logger_t* malloc_logger;

namespace
{
    std::atomic<pthread_t> watchedThread { nullptr };
    std::atomic<bool> allocationWatchArmed { false };
    std::atomic<uint64_t> audioThreadAllocations { 0 };

    // The first few offending call stacks, symbolised after the run.
    constexpr int maxRecordedStacks = 4;
    void* recordedStacks[maxRecordedStacks][24] {};
    int recordedDepth[maxRecordedStacks] {};
    uintptr_t recordedSize[maxRecordedStacks] {};

    void allocationHook (uint32_t type, uintptr_t, uintptr_t size, uintptr_t, uintptr_t, uint32_t)
    {
        constexpr uint32_t allocateFlag = 2;
        if ((type & allocateFlag) != 0 && allocationWatchArmed.load (std::memory_order_relaxed)
            && pthread_equal (pthread_self(), watchedThread.load (std::memory_order_relaxed)))
        {
            const auto index = audioThreadAllocations.fetch_add (1, std::memory_order_relaxed);
            if (index < maxRecordedStacks)
            {
                allocationWatchArmed.store (false, std::memory_order_relaxed); // backtrace() must not recurse into us
                recordedDepth[index] = backtrace (recordedStacks[index], 24);
                recordedSize[index] = size;
                allocationWatchArmed.store (true, std::memory_order_relaxed);
            }
        }
    }

    void printRecordedStacks (uint64_t count)
    {
        for (uint64_t i = 0; i < std::min<uint64_t> (count, maxRecordedStacks); ++i)
        {
            std::printf ("  audio-thread allocation #%llu (%lu bytes):\n", (unsigned long long) i, (unsigned long) recordedSize[i]);
            char** symbols = backtrace_symbols (recordedStacks[i], recordedDepth[i]);
            for (int f = 0; f < recordedDepth[i] && symbols != nullptr; ++f)
                std::printf ("      %s\n", symbols[f]);
            std::free (symbols);
        }
    }

    double ticksToNanoseconds()
    {
        mach_timebase_info_data_t info {};
        mach_timebase_info (&info);
        return static_cast<double> (info.numer) / static_cast<double> (info.denom);
    }

    const double nsPerTick = ticksToNanoseconds();

    double threadCpuMsNamed (const char* wanted)
    {
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t count = 0;
        double total = 0.0;

        if (task_threads (mach_task_self(), &threads, &count) != KERN_SUCCESS)
            return -1.0;

        for (mach_msg_type_number_t i = 0; i < count; ++i)
        {
            char name[64] {};
            if (auto pt = pthread_from_mach_thread_np (threads[i]))
                pthread_getname_np (pt, name, sizeof (name));

            if (std::strcmp (name, wanted) == 0)
            {
                thread_basic_info_data_t info {};
                mach_msg_type_number_t infoCount = THREAD_BASIC_INFO_COUNT;
                if (thread_info (threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t> (&info), &infoCount) == KERN_SUCCESS)
                    total += info.user_time.seconds * 1000.0 + info.user_time.microseconds / 1000.0
                           + info.system_time.seconds * 1000.0 + info.system_time.microseconds / 1000.0;
            }
            mach_port_deallocate (mach_task_self(), threads[i]);
        }

        vm_deallocate (mach_task_self(), reinterpret_cast<vm_address_t> (threads), count * sizeof (thread_act_t));
        return total;
    }

    int countThreadsNamed (const char* wanted)
    {
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t count = 0;
        int found = 0;

        if (task_threads (mach_task_self(), &threads, &count) != KERN_SUCCESS)
            return -1;

        for (mach_msg_type_number_t i = 0; i < count; ++i)
        {
            char name[64] {};
            if (auto pt = pthread_from_mach_thread_np (threads[i]))
                pthread_getname_np (pt, name, sizeof (name));
            if (std::strcmp (name, wanted) == 0)
                ++found;
            mach_port_deallocate (mach_task_self(), threads[i]);
        }

        vm_deallocate (mach_task_self(), reinterpret_cast<vm_address_t> (threads), count * sizeof (thread_act_t));
        return found;
    }

    //==========================================================================
    struct Check
    {
        std::string name;
        bool pass;
        std::string detail;
    };

    std::vector<Check> checks;

    void check (const std::string& name, bool pass, const std::string& detail = {})
    {
        checks.push_back ({ name, pass, detail });
        std::printf ("  [%s] %s%s%s\n", pass ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
        std::fflush (stdout);
    }

    void pump (int ms)
    {
        MessageManager::getInstance()->runDispatchLoopUntil (ms);
    }

    template <typename Predicate>
    bool pumpUntil (Predicate p, int timeoutMs)
    {
        const auto deadline = Time::getMillisecondCounter() + static_cast<uint32> (timeoutMs);
        while (Time::getMillisecondCounter() < deadline)
        {
            if (p())
                return true;
            pump (5);
        }
        return p();
    }

    uint16_t volume14ForDb (double db)
    {
        return static_cast<uint16_t> (faderPositionToVolume14 (decibelsToFaderPosition (db)));
    }

    //==========================================================================
    /** The companion's side of the wire, minimal but real: UDP on 127.0.0.1. */
    class FakeCompanion
    {
    public:
        struct Instance
        {
            uint64_t busId = 0;
            std::string name;
            bool hasColour = false;
            uint32_t colour = 0;
            int channels = 0;
            bool hasState = false;
            wire::StateRecord state {};
            uint64_t states = 0;
            bool gone = false;
        };

        ~FakeCompanion() { stop(); }

        bool start (uint16_t wantedPort)
        {
            stop();
            fd = ::socket (AF_INET, SOCK_DGRAM, 0);
            const int one = 1;
            ::setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));
            const int big = 4 * 1024 * 1024;
            ::setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof (big));
            sockaddr_in a {};
            a.sin_len = sizeof (a);
            a.sin_family = AF_INET;
            a.sin_port = htons (wantedPort);
            a.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
            if (::bind (fd, reinterpret_cast<sockaddr*> (&a), sizeof (a)) != 0)
            {
                ::close (fd);
                fd = -1;
                return false;
            }
            socklen_t len = sizeof (a);
            ::getsockname (fd, reinterpret_cast<sockaddr*> (&a), &len);
            port = ntohs (a.sin_port);
            session = randomNonZeroId();
            running = true;
            thread = std::thread ([this] { run(); });
            return true;
        }

        void stop()
        {
            running = false;
            if (thread.joinable())
                thread.join();
            if (fd >= 0)
                ::close (fd);
            fd = -1;
        }

        void forget()
        {
            const std::lock_guard lock (mutex);
            instances.clear();
            buses.clear();
            hellos = infos = stateDatagrams = byes = 0;
        }

        std::map<uint64_t, Instance> snapshot() const
        {
            const std::lock_guard lock (mutex);
            return instances;
        }

        uint64_t idForName (const std::string& name) const
        {
            const std::lock_guard lock (mutex);
            for (const auto& [id, inst] : instances)
                if (inst.name == name && ! inst.gone)
                    return id;
            return 0;
        }

        std::vector<uint64_t> idsNamed (const std::string& name) const
        {
            const std::lock_guard lock (mutex);
            std::vector<uint64_t> ids;
            for (const auto& [id, inst] : instances)
                if (inst.name == name)
                    ids.push_back (id);
            return ids;
        }

        bool sendControl (uint64_t instanceId, uint8_t mask, uint16_t volume14, int8_t pan, bool muted)
        {
            wire::Message m;
            m.type = wire::MessageType::control;
            wire::ControlRecord rec;
            rec.instanceId = instanceId;
            rec.mask = mask;
            rec.volume14 = (mask & wire::controlMask::volume) != 0 ? volume14 : 0;
            rec.pan = (mask & wire::controlMask::pan) != 0 ? pan : 0;
            rec.muted = (mask & wire::controlMask::mute) != 0 && muted;
            {
                const std::lock_guard lock (mutex);
                rec.seq = ++seqs[instanceId];
            }
            m.controls.push_back (rec);
            return sendToOwner (instanceId, m);
        }

        bool sendLease (uint64_t instanceId, bool mute, uint16_t durationMs)
        {
            wire::Message m;
            m.type = wire::MessageType::lease;
            m.leases.push_back ({ instanceId, mute, durationMs });
            return sendToOwner (instanceId, m);
        }

        /** Garbage to every bus: noise, and near-valid datagrams with the right magic. */
        int sendGarbage (int perBus, unsigned seed)
        {
            std::vector<std::pair<sockaddr_in, uint64_t>> targets;
            {
                const std::lock_guard lock (mutex);
                for (const auto& [id, bus] : buses)
                    targets.emplace_back (bus, id);
            }

            std::mt19937 rng (seed);
            int sent = 0;

            for (int i = 0; i < perBus; ++i)
            {
                for (auto& [address, busId] : targets)
                {
                    std::vector<uint8_t> junk (rng() % 1400);
                    for (auto& b : junk)
                        b = static_cast<uint8_t> (rng());

                    if (i % 2 == 0 && junk.size() >= 24)
                    {
                        const uint8_t header[] = { 'M', 'X', 'S', 'N', 1, static_cast<uint8_t> (0x10 + rng() % 4) };
                        std::memcpy (junk.data(), header, sizeof (header));
                        for (int k = 0; k < 8; ++k)
                            junk[16 + static_cast<size_t> (k)] = static_cast<uint8_t> (busId >> (8 * k)); // right target
                    }

                    ::sendto (fd, junk.data(), junk.size(), 0, reinterpret_cast<const sockaddr*> (&address), sizeof (address));
                    ++sent;
                }

                if (i % 32 == 31)
                    std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
            return sent;
        }

        std::atomic<uint64_t> hellos { 0 }, infos { 0 }, stateDatagrams { 0 }, byes { 0 }, rejected { 0 };
        uint16_t port = 0;
        uint64_t session = 0;

    private:
        bool sendToOwner (uint64_t instanceId, wire::Message& m)
        {
            sockaddr_in address {};
            {
                const std::lock_guard lock (mutex);
                const auto it = instances.find (instanceId);
                if (it == instances.end())
                    return false;
                const auto bus = buses.find (it->second.busId);
                if (bus == buses.end())
                    return false;
                address = bus->second;
                m.targetBusId = it->second.busId;
            }
            m.senderId = session;
            std::vector<uint8_t> bytes;
            if (! wire::encode (m, bytes))
                return false;
            return ::sendto (fd, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*> (&address), sizeof (address)) > 0;
        }

        void run()
        {
            std::vector<uint8_t> buffer (65536);
            wire::Message m;

            while (running)
            {
                pollfd p { fd, POLLIN, 0 };
                if (::poll (&p, 1, 20) <= 0)
                    continue;

                for (int burst = 0; burst < 512; ++burst)
                {
                    sockaddr_in from {};
                    socklen_t len = sizeof (from);
                    const auto n = ::recvfrom (fd, buffer.data(), buffer.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*> (&from), &len);
                    if (n <= 0)
                        break;

                    if (wire::decode (buffer.data(), static_cast<size_t> (n), m) != wire::DecodeError::none)
                    {
                        rejected.fetch_add (1);
                        continue;
                    }

                    handle (m, from);
                }
            }
        }

        void handle (const wire::Message& m, const sockaddr_in& from)
        {
            const std::lock_guard lock (mutex);

            switch (m.type)
            {
                case wire::MessageType::hello:
                {
                    ++hellos;
                    buses[m.senderId] = from;
                    wire::Message welcome;
                    welcome.type = wire::MessageType::welcome;
                    welcome.senderId = session;
                    welcome.targetBusId = m.senderId;
                    std::vector<uint8_t> bytes;
                    if (wire::encode (welcome, bytes))
                        ::sendto (fd, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*> (&from), sizeof (from));
                    break;
                }
                case wire::MessageType::info:
                    ++infos;
                    buses[m.senderId] = from;
                    for (const auto& r : m.info)
                    {
                        auto& inst = instances[r.instanceId];
                        inst.busId = m.senderId;
                        inst.name = std::string (r.name.view());
                        inst.hasColour = r.hasColour;
                        inst.colour = r.colourArgb;
                        inst.channels = r.channels;
                        inst.gone = false;
                    }
                    break;
                case wire::MessageType::state:
                    ++stateDatagrams;
                    buses[m.senderId] = from;
                    for (const auto& r : m.states)
                    {
                        auto& inst = instances[r.instanceId];
                        inst.busId = m.senderId;
                        inst.state = r;
                        inst.hasState = true;
                        ++inst.states;
                        inst.gone = false;
                    }
                    break;
                case wire::MessageType::bye:
                    ++byes;
                    for (const auto id : m.ids)
                        instances[id].gone = true;
                    if (m.ids.empty())
                        for (auto& [id, inst] : instances)
                            if (inst.busId == m.senderId)
                                inst.gone = true;
                    break;
                case wire::MessageType::welcome:
                case wire::MessageType::control:
                case wire::MessageType::lease:
                case wire::MessageType::reassign:
                    break;
            }
        }

        mutable std::mutex mutex;
        std::map<uint64_t, Instance> instances;
        std::map<uint64_t, sockaddr_in> buses;
        std::map<uint64_t, uint16_t> seqs;
        int fd = -1;
        std::atomic<bool> running { false };
        std::thread thread;
    };

    //==========================================================================
    /** Counts host-side parameter traffic, to prove remote edits arrive as
        gestures on the message thread and never from the audio thread. */
    class ParameterSpy final : public AudioProcessorListener
    {
    public:
        void audioProcessorParameterChanged (AudioProcessor*, int, float) override
        {
            ++changes;
            if (! MessageManager::getInstance()->isThisTheMessageThread())
                ++offMessageThread;
        }
        void audioProcessorChanged (AudioProcessor*, const ChangeDetails&) override {}
        void audioProcessorParameterChangeGestureBegin (AudioProcessor*, int) override { ++begins; }
        void audioProcessorParameterChangeGestureEnd (AudioProcessor*, int) override { ++ends; }

        std::atomic<int> changes { 0 }, begins { 0 }, ends { 0 }, offMessageThread { 0 };
    };

    //==========================================================================
    class AudioRunner final : public Thread
    {
    public:
        AudioRunner (std::vector<AudioPluginInstance*> list, int blockSizeToUse, double rateToUse, int totalBlocksToRun, bool realtimeToUse)
            : Thread ("harness audio"),
              instances (std::move (list)), blockSize (blockSizeToUse), rate (rateToUse),
              totalBlocks (totalBlocksToRun), realtime (realtimeToUse),
              lastLeft (instances.size()), lastRight (instances.size())
        {
            buffers.reserve (instances.size());
            for (size_t i = 0; i < instances.size(); ++i)
                buffers.emplace_back (2, blockSize);
            blockTimesNs.resize (static_cast<size_t> (totalBlocks), 0.0);
            midi.ensureSize (256);
        }

        void run() override
        {
            watchedThread.store (pthread_self());

            // Self-test of the allocation counter on this very thread.
            // (The pointer escapes through the asm, so the compiler cannot
            // elide the new/delete pair as C++ allows it to.)
            allocationWatchArmed.store (true);
            auto* probe = new char[32];
            asm volatile ("" : : "r" (probe) : "memory");
            allocationWatchArmed.store (false);
            delete[] probe;
            selfTestAllocations = audioThreadAllocations.exchange (0);
            recordedDepth[0] = 0; // forget the self-test's own stack

            thread_basic_info_data_t info {};
            const auto cpuStart = threadCpuNs();
            const auto periodTicks = static_cast<uint64_t> ((blockSize / rate) * 1.0e9 / nsPerTick);
            auto deadline = mach_absolute_time();
            juce::ignoreUnused (info);

            for (int block = 0; block < totalBlocks && ! threadShouldExit(); ++block)
            {
                if (realtime)
                {
                    deadline += periodTicks;
                    mach_wait_until (deadline);
                }

                for (auto& b : buffers)
                {
                    FloatVectorOperations::fill (b.getWritePointer (0), 0.5f, blockSize);
                    FloatVectorOperations::fill (b.getWritePointer (1), 0.25f, blockSize);
                }

                const auto t0 = mach_absolute_time();
                allocationWatchArmed.store (true, std::memory_order_relaxed);

                for (size_t i = 0; i < instances.size(); ++i)
                {
                    midi.clear();
                    instances[i]->processBlock (buffers[i], midi);
                }

                allocationWatchArmed.store (false, std::memory_order_relaxed);
                const auto t1 = mach_absolute_time();
                blockTimesNs[static_cast<size_t> (block)] = static_cast<double> (t1 - t0) * nsPerTick;

                for (size_t i = 0; i < instances.size(); ++i)
                {
                    lastLeft[i].store (buffers[i].getSample (0, blockSize - 1), std::memory_order_relaxed);
                    lastRight[i].store (buffers[i].getSample (1, blockSize - 1), std::memory_order_relaxed);
                }

                if (realtime && mach_absolute_time() > deadline + periodTicks)
                    overruns.fetch_add (1);

                if (block == 0)
                    firstBlockAllocations = audioThreadAllocations.load();

                blocksDone.store (block + 1, std::memory_order_release);
            }

            cpuNs = threadCpuNs() - cpuStart;
        }

        static double threadCpuNs()
        {
            thread_basic_info_data_t info {};
            mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
            const auto self = mach_thread_self();
            thread_info (self, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t> (&info), &count);
            mach_port_deallocate (mach_task_self(), self);
            return (info.user_time.seconds + info.system_time.seconds) * 1.0e9
                 + (info.user_time.microseconds + info.system_time.microseconds) * 1.0e3;
        }

        float left (size_t i) const { return lastLeft[i].load (std::memory_order_relaxed); }
        float right (size_t i) const { return lastRight[i].load (std::memory_order_relaxed); }

        std::vector<AudioPluginInstance*> instances;
        const int blockSize;
        const double rate;
        const int totalBlocks;
        const bool realtime;
        std::vector<AudioBuffer<float>> buffers;
        MidiBuffer midi;
        std::vector<std::atomic<float>> lastLeft, lastRight;
        std::vector<double> blockTimesNs;
        std::atomic<int> blocksDone { 0 };
        std::atomic<int> overruns { 0 };
        double cpuNs = 0.0;
        uint64_t selfTestAllocations = 0;
        uint64_t firstBlockAllocations = 0;
    };

    //==========================================================================
    struct Options
    {
        String pluginPath;
        int instances = 128;
        double seconds = 30.0;
        int blockSize = 64;
        double rate = 48000.0;
        bool companion = true;
        bool realtime = true;
        bool steady = false;
        String e2eUrl;
        String pairingCode;
        int udpPort = 0;
    };

    Options parse (int argc, char** argv)
    {
        Options o;
        for (int i = 1; i < argc; ++i)
        {
            const String a (argv[i]);
            const auto next = [&] { return i + 1 < argc ? String (argv[++i]) : String(); };
            if (a == "--plugin") o.pluginPath = next();
            else if (a == "--instances") o.instances = next().getIntValue();
            else if (a == "--seconds") o.seconds = next().getDoubleValue();
            else if (a == "--block") o.blockSize = next().getIntValue();
            else if (a == "--rate") o.rate = next().getDoubleValue();
            else if (a == "--no-companion") o.companion = false;
            else if (a == "--fast") o.realtime = false;
            else if (a == "--steady") o.steady = true;
            else if (a == "--e2e-http") o.e2eUrl = next();
            else if (a == "--pairing-code") o.pairingCode = next();
            else if (a == "--udp-port") o.udpPort = next().getIntValue();
        }
        return o;
    }

    std::unique_ptr<AudioPluginFormat> formatFor (const String& path)
    {
        if (path.endsWithIgnoreCase (".component"))
            return std::make_unique<AudioUnitPluginFormat>();
        return std::make_unique<VST3PluginFormat>();
    }

    std::unique_ptr<AudioPluginInstance> createInstance (AudioPluginFormat& format, const PluginDescription& desc,
                                                         double rate, int block, String& error)
    {
        auto instance = format.createInstanceFromDescription (desc, rate, block, error);
        if (instance == nullptr)
            return nullptr;

        instance->enableAllBuses();
        instance->setPlayConfigDetails (2, 2, rate, block);
        instance->prepareToPlay (rate, block);
        return instance;
    }

    AudioProcessorParameter* findParameter (AudioPluginInstance& instance, const String& name)
    {
        for (auto* p : instance.getParameters())
            if (p->getName (64).equalsIgnoreCase (name))
                return p;
        return nullptr;
    }

    void nameInstance (AudioPluginInstance& instance, const String& name, std::optional<uint32> colour)
    {
        AudioProcessor::TrackProperties props;
        props.name = name;
        props.colourARGB = colour;
        instance.updateTrackProperties (props);
    }

    String instanceName (size_t i) { return "Track " + String (static_cast<int> (i)).paddedLeft ('0', 3); }

    struct Stats
    {
        double mean = 0, p50 = 0, p99 = 0, p999 = 0, max = 0;
    };

    Stats summarise (std::vector<double> values)
    {
        Stats s;
        if (values.empty())
            return s;
        std::sort (values.begin(), values.end());
        double sum = 0;
        for (auto v : values)
            sum += v;
        s.mean = sum / static_cast<double> (values.size());
        const auto at = [&] (double q) { return values[std::min (values.size() - 1, static_cast<size_t> (q * static_cast<double> (values.size())))]; };
        s.p50 = at (0.5);
        s.p99 = at (0.99);
        s.p999 = at (0.999);
        s.max = values.back();
        return s;
    }

    //==========================================================================
    // Minimal HTTP client against the real companion (e2e mode).
    //==========================================================================
    var httpJson (const String& base, const String& path, const String& method, const String& token,
                  const String& body, int& status)
    {
        auto url = URL (base + path);
        if (body.isNotEmpty())
            url = url.withPOSTData (body);

        String headers = "Content-Type: application/json\r\n";
        if (token.isNotEmpty())
            headers << "Authorization: Bearer " << token << "\r\n";

        auto options = URL::InputStreamOptions (body.isNotEmpty() ? URL::ParameterHandling::inPostData
                                                                  : URL::ParameterHandling::inAddress)
                           .withExtraHeaders (headers)
                           .withConnectionTimeoutMs (3000)
                           .withStatusCode (&status)
                           .withHttpRequestCmd (method);

        std::unique_ptr<InputStream> stream (url.createInputStream (options));
        if (stream == nullptr)
            return {};
        return JSON::parse (stream->readEntireStreamAsString());
    }
}

//==============================================================================
static int runE2E (const Options& o, AudioPluginFormat& format, const PluginDescription& desc)
{
    std::printf ("\nE2E against the real companion at %s (udp %d)\n\n", o.e2eUrl.toRawUTF8(), o.udpPort);

    constexpr int count = 4;
    std::vector<std::unique_ptr<AudioPluginInstance>> owned;
    std::vector<AudioPluginInstance*> list;
    String error;

    for (int i = 0; i < count; ++i)
    {
        auto inst = createInstance (format, desc, o.rate, o.blockSize, error);
        if (inst == nullptr)
        {
            std::printf ("could not create instance: %s\n", error.toRawUTF8());
            return 1;
        }
        nameInstance (*inst, "E2E " + String (i), 0xff000000u | static_cast<uint32> (0x203040 * (i + 1)));
        list.push_back (inst.get());
        owned.push_back (std::move (inst));
    }

    AudioRunner audio (list, o.blockSize, o.rate, static_cast<int> (o.rate / o.blockSize * 20.0), true);
    audio.startRealtimeThread (Thread::RealtimeOptions().withPeriodMs (o.blockSize * 1000.0 / o.rate));

    int status = 0;
    const auto pair = httpJson (o.e2eUrl, "/api/pair", "POST", {}, "{\"code\":\"" + o.pairingCode + "\"}", status);
    const auto token = pair["sessionToken"].toString();
    check ("paired with the companion", status == 201 && token.isNotEmpty(), "HTTP " + String (status).toStdString());

    std::map<String, String> idByName;
    var channels;
    const bool listed = pumpUntil ([&] {
        const auto state = httpJson (o.e2eUrl, "/api/plugin/state", "GET", token, {}, status);
        channels = state["channels"];
        idByName.clear();
        if (auto* arr = channels.getArray())
            for (const auto& ch : *arr)
                if (static_cast<bool> (ch["online"]))
                    idByName[ch["displayName"].toString()] = ch["instanceId"].toString();
        return idByName.size() >= count;
    }, 6000);
    check ("companion lists all instances by host track name", listed, std::to_string (idByName.size()) + " online");

    if (auto* arr = channels.getArray())
        for (const auto& ch : *arr)
            std::printf ("      %s  %-8s  v14=%s pan=%s colour=%s\n", ch["instanceId"].toString().toRawUTF8(),
                         ch["displayName"].toString().toRawUTF8(), ch["faderValue14"].toString().toRawUTF8(),
                         ch["pan"].toString().toRawUTF8(), ch["colour"].toString().toRawUTF8());

    const auto write = [&] (const String& name, const String& field, int value) {
        int s = 0;
        httpJson (o.e2eUrl, "/api/plugin/write", "POST", token,
                  "{\"instanceId\":\"" + idByName[name] + "\",\"field\":\"" + field + "\",\"value\":" + String (value) + "}", s);
        return s;
    };

    const int v14 = volume14ForDb (-12.0);
    check ("POST volume -12 dB accepted", write ("E2E 0", "volume", v14) == 204);
    check ("POST pan hard left accepted", write ("E2E 1", "pan", -100) == 204);
    check ("POST mute accepted", write ("E2E 2", "mute", 1) == 204);

    const float g12 = std::pow (10.0f, -12.0f / 20.0f);
    const auto writtenAt = Time::getMillisecondCounterHiRes();
    check ("E2E 0 output follows the surface fader (-12 dB)",
           pumpUntil ([&] { return std::abs (audio.left (0) - 0.5f * g12) < 1e-4f && std::abs (audio.right (0) - 0.25f * g12) < 1e-4f; }, 3000),
           "L=" + std::to_string (audio.left (0)) + ", HTTP write to audio within "
               + std::to_string (static_cast<int> (Time::getMillisecondCounterHiRes() - writtenAt)) + " ms");
    check ("E2E 1 output follows the surface pan (hard left)",
           pumpUntil ([&] { return std::abs (audio.left (1) - 0.5f) < 1e-6f && audio.right (1) == 0.0f; }, 3000));
    check ("E2E 2 output silenced by the surface mute", pumpUntil ([&] { return audio.left (2) == 0.0f && audio.right (2) == 0.0f; }, 3000));

    check ("POST solo accepted", write ("E2E 3", "solo", 1) == 204);
    check ("solo on E2E 3 lease-mutes the others (E2E 0 and 1 silent), not E2E 3",
           pumpUntil ([&] { return audio.left (0) == 0.0f && audio.left (1) == 0.0f && std::abs (audio.left (3) - 0.5f) < 1e-6f; }, 3000));

    const auto state = httpJson (o.e2eUrl, "/api/plugin/state", "GET", token, {}, status);
    bool confirmed = false;
    if (auto* arr = state["channels"].getArray())
        for (const auto& ch : *arr)
            if (ch["instanceId"].toString() == idByName["E2E 0"])
                confirmed = static_cast<int> (ch["faderValue14"]) == v14 && ! ch.hasProperty ("pending");
    check ("companion view confirms the write (faderValue14, no pending)", confirmed);

    check ("solo off restores E2E 0 and 1 to their own settings", write ("E2E 3", "solo", 0) == 204
           && pumpUntil ([&] { return std::abs (audio.left (0) - 0.5f * g12) < 1e-4f && std::abs (audio.left (1) - 0.5f) < 1e-6f
                                      && audio.right (1) == 0.0f; }, 3000));

    // Solo again, then the companion is killed by the script driving this
    // run (see docs/MIXER_SEND.md): the harness just reports what it sees.
    check ("solo engaged again for the companion-kill check", write ("E2E 3", "solo", 1) == 204
           && pumpUntil ([&] { return audio.left (0) == 0.0f; }, 3000));
    std::printf ("  READY-FOR-KILL\n");
    std::fflush (stdout);
    const bool relinquished = pumpUntil ([&] { return std::abs (audio.left (0) - 0.5f * g12) < 1e-4f; }, 8000);
    check ("after the companion process is killed, the solo lease lapses and E2E 0 is audible again", relinquished);

    audio.signalThreadShouldExit();
    audio.stopThread (4000);
    for (auto& inst : owned)
        inst->releaseResources();
    owned.clear();

    int failed = 0;
    for (const auto& c : checks)
        failed += c.pass ? 0 : 1;
    std::printf ("\n%s: %d/%zu checks passed\n", failed == 0 ? "E2E PASS" : "E2E FAIL", static_cast<int> (checks.size()) - failed, checks.size());
    return failed == 0 ? 0 : 1;
}

//==============================================================================
int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI juceInit;
    const auto o = parse (argc, argv);

    if (o.pluginPath.isEmpty())
    {
        std::fprintf (stderr, "usage: mixer_send_harness --plugin <path.vst3|path.component> [options]\n");
        return 2;
    }

    // Route the plugin's bus to our fake companion (read once, when the first
    // instance creates the bus). A real companion on 4891 is left alone.
    FakeCompanion companion;
    if (o.e2eUrl.isNotEmpty())
    {
        ::setenv ("MIXER_SEND_COMPANION_PORT", String (o.udpPort).toRawUTF8(), 1);
    }
    else
    {
        if (! companion.start (0))
        {
            std::fprintf (stderr, "could not bind the fake companion\n");
            return 2;
        }
        ::setenv ("MIXER_SEND_COMPANION_PORT", String (companion.port).toRawUTF8(), 1);
        if (! o.companion)
            companion.stop(); // the port stays reserved for nobody: sends go nowhere
    }

    malloc_logger = allocationHook;

    auto format = formatFor (o.pluginPath);
    OwnedArray<PluginDescription> found;
    format->findAllTypesForFile (found, o.pluginPath);

    if (found.isEmpty())
    {
        std::fprintf (stderr, "no plugin found in %s\n", o.pluginPath.toRawUTF8());
        return 2;
    }

    const auto& desc = *found[0];
    std::printf ("Mixer SEND harness\n  plugin   %s (%s, %s)\n", desc.name.toRawUTF8(), desc.pluginFormatName.toRawUTF8(),
                 o.pluginPath.toRawUTF8());

    if (o.e2eUrl.isNotEmpty())
        return runE2E (o, *format, desc);

    std::printf ("  run      %d instances, %.0f Hz / %d-sample blocks, %.0f s of audio, %s, companion %s (udp %d)\n\n",
                 o.instances, o.rate, o.blockSize, o.seconds, o.realtime ? "real-time paced" : "as fast as possible",
                 o.companion ? "fake" : "ABSENT", companion.port);

    // --- instantiate -----------------------------------------------------
    std::vector<std::unique_ptr<AudioPluginInstance>> owned;
    std::vector<AudioPluginInstance*> list;
    String error;
    const auto createStart = Time::getMillisecondCounterHiRes();

    for (int i = 0; i < o.instances; ++i)
    {
        auto instance = createInstance (*format, desc, o.rate, o.blockSize, error);
        if (instance == nullptr)
        {
            std::fprintf (stderr, "instance %d failed: %s\n", i, error.toRawUTF8());
            return 1;
        }
        // Even tracks get a name and colour from the "host", odd ones only a name.
        nameInstance (*instance, instanceName (static_cast<size_t> (i)),
                      i % 2 == 0 ? std::optional<uint32> (0xff000000u | static_cast<uint32> (i * 0x010203)) : std::nullopt);
        list.push_back (instance.get());
        owned.push_back (std::move (instance));
    }

    std::printf ("  created %d instances in %.0f ms; bus threads in process: %d\n\n", o.instances,
                 Time::getMillisecondCounterHiRes() - createStart, countThreadsNamed ("Mixer SEND bus"));

    ParameterSpy spy;
    for (auto* inst : list)
        inst->addListener (&spy);

    const int totalBlocks = static_cast<int> (std::ceil (o.seconds * o.rate / o.blockSize));
    AudioRunner audio (list, o.blockSize, o.rate, totalBlocks, o.realtime);
    const double busCpuBefore = threadCpuMsNamed ("Mixer SEND bus");
    const auto wallStart = Time::getMillisecondCounterHiRes();
    audio.startRealtimeThread (Thread::RealtimeOptions().withPeriodMs (o.blockSize * 1000.0 / o.rate));

    const auto elapsed = [&] { return (Time::getMillisecondCounterHiRes() - wallStart) / 1000.0; };

    // Bus CPU per phase of the timeline, so a stress phase is not mistaken
    // for the steady-state cost.
    double phaseCpu = threadCpuMsNamed ("Mixer SEND bus");
    double phaseStart = 0.0;
    const auto phase = [&] (const char* name) {
        const double cpu = threadCpuMsNamed ("Mixer SEND bus");
        const double t = elapsed();
        std::printf ("      bus CPU %-34s %6.1f ms over %4.1f s = %.3f %% of one core\n", name, cpu - phaseCpu, t - phaseStart,
                     (t - phaseStart) > 0 ? 100.0 * (cpu - phaseCpu) / ((t - phaseStart) * 1000.0) : 0.0);
        phaseCpu = cpu;
        phaseStart = t;
    };
    const auto waitUntil = [&] (double t) { while (elapsed() < t && audio.isThreadRunning()) pump (10); };
    const float g12 = std::pow (10.0f, -12.0f / 20.0f);

    if (o.companion && o.realtime && ! o.steady)
    {
        // --- 1. every instance reported, with the host's names/colours -------
        const bool allSeen = pumpUntil ([&] {
            int n = 0;
            for (size_t i = 0; i < list.size(); ++i)
                n += companion.idForName (instanceName (i).toStdString()) != 0 ? 1 : 0;
            return n == o.instances;
        }, 4000);
        const auto snap = companion.snapshot();
        int named = 0, coloured = 0, withState = 0;
        for (const auto& [id, inst] : snap)
        {
            named += inst.name.rfind ("Track ", 0) == 0 ? 1 : 0;
            coloured += inst.hasColour ? 1 : 0;
            withState += inst.hasState ? 1 : 0;
        }
        check ("companion hears every instance with its host track name", allSeen,
               std::to_string (named) + " named, " + std::to_string (coloured) + " coloured, " + std::to_string (withState) + " with state");
        check ("one bus thread for all instances", countThreadsNamed ("Mixer SEND bus") == 1,
               std::to_string (countThreadsNamed ("Mixer SEND bus")) + " bus thread(s)");

        const auto id0 = companion.idForName ("Track 000");
        pumpUntil ([&] { return companion.snapshot()[id0].state.peakL != 0; }, 1000);
        const auto s0 = companion.snapshot()[id0].state;
        check ("meters and parameters reported (unity, centre, 0.5/0.25 DC in)",
               s0.volume14 == 12451 && s0.pan == 0 && s0.peakL == 188 && s0.peakR == 176 && s0.rmsL == 188
                   && (s0.flags & wire::stateFlag::processing) != 0,
               "v14=" + std::to_string (s0.volume14) + " peakL=" + std::to_string (s0.peakL) + " peakR=" + std::to_string (s0.peakR));

        // --- 2. remote volume / pan / mute ---------------------------------
        waitUntil (3.0);
        phase ("0-3 s  connect + steady");
        const auto id1 = companion.idForName ("Track 001");
        const auto id2 = companion.idForName ("Track 002");
        const auto id3 = companion.idForName ("Track 003");
        const auto sentAt = Time::getMillisecondCounterHiRes();
        companion.sendControl (id0, wire::controlMask::volume, volume14ForDb (-12.0), 0, false);
        companion.sendControl (id1, wire::controlMask::pan, 0, -100, false);
        companion.sendControl (id2, wire::controlMask::mute, 0, 0, true);
        companion.sendControl (id3, wire::controlMask::volume | wire::controlMask::pan, 0, 50, false);

        const bool volumeLanded = pumpUntil ([&] { return std::abs (audio.left (0) - 0.5f * g12) < 1e-5f && std::abs (audio.right (0) - 0.25f * g12) < 1e-5f; }, 2000);
        const auto latencyMs = Time::getMillisecondCounterHiRes() - sentAt;
        check ("remote volume -12 dB changes the output exactly", volumeLanded,
               "L=" + std::to_string (audio.left (0)) + " expected " + std::to_string (0.5f * g12) + ", landed within " + std::to_string (static_cast<int> (latencyMs)) + " ms incl. 20 ms ramp");
        check ("remote pan hard left: L unity, R silent",
               pumpUntil ([&] { return audio.left (1) == 0.5f && audio.right (1) == 0.0f; }, 2000),
               "L=" + std::to_string (audio.left (1)) + " R=" + std::to_string (audio.right (1)));
        check ("remote mute silences the output", pumpUntil ([&] { return audio.left (2) == 0.0f && audio.right (2) == 0.0f; }, 2000));
        check ("remote -inf silences the output (volume 0)", pumpUntil ([&] { return audio.left (3) == 0.0f && audio.right (3) == 0.0f; }, 2000));

        auto* volumeParam = findParameter (*list[0], "Volume");
        auto* panParam = findParameter (*list[1], "Pan");
        check ("the HOST's parameters moved (not just the audio)",
               volumeParam != nullptr && panParam != nullptr
                   && std::abs (volumeParam->getValue() - static_cast<float> (volume14ToFaderPosition (volume14ForDb (-12.0)))) < 1e-4f
                   && panParam->getValue() < 1e-4f,
               volumeParam != nullptr ? "volume param " + volumeParam->getCurrentValueAsText().toStdString() : "no param");
        pump (400); // let the 250 ms gesture window close
        check ("remote edits arrive as balanced gestures on the message thread",
               spy.begins.load() >= 4 && spy.begins.load() == spy.ends.load() && spy.offMessageThread.load() == 0,
               "begins=" + std::to_string (spy.begins.load()) + " ends=" + std::to_string (spy.ends.load()) + " changes=" + std::to_string (spy.changes.load()));

        const bool acked = pumpUntil ([&] {
            auto snapNow = companion.snapshot();
            return snapNow[id0].state.ackSeq == 1 && snapNow[id0].state.volume14 == volume14ForDb (-12.0)
                && snapNow[id2].state.ackSeq == 1 && (snapNow[id2].state.flags & wire::stateFlag::muted) != 0;
        }, 2000);
        check ("state echoes the applied values and ackSeq", acked);

        // --- 3. a lease that is not refreshed must lapse -------------------
        waitUntil (5.0);
        const auto id4 = companion.idForName ("Track 004");
        companion.sendLease (id4, true, 1000);
        const bool leaseMuted = pumpUntil ([&] { return audio.left (4) == 0.0f; }, 1000);
        const auto leaseStart = Time::getMillisecondCounterHiRes();
        const bool leaseLapsed = pumpUntil ([&] { return audio.left (4) == 0.5f; }, 3000);
        const auto heldMs = Time::getMillisecondCounterHiRes() - leaseStart;
        check ("solo lease mutes, then lapses by itself when not refreshed", leaseMuted && leaseLapsed && heldMs < 1600,
               "held " + std::to_string (static_cast<int> (heldMs)) + " ms of a 1000 ms lease");
        auto p4 = findParameter (*list[4], "Mute");
        check ("a lease never touches the saved mute parameter", p4 != nullptr && p4->getValue() < 0.5f);

        // --- 4. garbage flood -----------------------------------------------
        waitUntil (7.0);
        phase ("3-7 s  remote edits + lease");
        const int garbage = companion.sendGarbage (3000, 42);
        const auto id5 = companion.idForName ("Track 005");
        companion.sendControl (id5, wire::controlMask::volume, volume14ForDb (-12.0), 0, false);
        check ("after a garbage flood valid controls still land", pumpUntil ([&] { return std::abs (audio.left (5) - 0.5f * g12) < 1e-5f; }, 3000),
               std::to_string (garbage) + " garbage datagrams sent");

        // --- 5. companion dies ------------------------------------------------
        waitUntil (10.0);
        phase ("7-10 s garbage flood (3000)");
        const auto beforeDeath = audio.blocksDone.load();
        companion.stop();
        waitUntil (14.5);
        phase ("10-14.5 s companion absent");
        const auto afterDeath = audio.blocksDone.load();
        check ("companion absent: audio keeps running, remote settings kept",
               afterDeath - beforeDeath > static_cast<int> (4.0 * o.rate / o.blockSize)
                   && std::abs (audio.left (0) - 0.5f * g12) < 1e-5f && audio.left (2) == 0.0f,
               std::to_string (afterDeath - beforeDeath) + " blocks while absent");

        // --- 6. companion restarts on the same port, new session ------------
        const auto oldPort = companion.port;
        companion.forget();
        const bool restarted = companion.start (oldPort);
        const auto restartAt = Time::getMillisecondCounterHiRes();
        const bool relearned = restarted && pumpUntil ([&] {
            int n = 0;
            for (const auto& [id, inst] : companion.snapshot())
                n += inst.hasState && ! inst.name.empty() ? 1 : 0;
            return n >= o.instances;
        }, 4000);
        check ("restarted companion relearns every instance (names + state)", relearned,
               std::to_string (static_cast<int> (Time::getMillisecondCounterHiRes() - restartAt)) + " ms after restart");
        const auto id6 = companion.idForName ("Track 006");
        companion.sendControl (id6, wire::controlMask::pan, 0, 100, false); // seq 1 again, new session
        check ("new companion session is obeyed from seq 1",
               pumpUntil ([&] { return audio.left (6) == 0.0f && audio.right (6) == 0.25f; }, 2000));

        // --- 7. churn + a duplicated instance -------------------------------
        waitUntil (17.0);
        phase ("14.5-17 s companion restart");
        MemoryBlock state0;
        list[0]->getStateInformation (state0);

        auto duplicate = createInstance (*format, desc, o.rate, o.blockSize, error);
        bool dupOk = duplicate != nullptr;
        if (dupOk)
        {
            duplicate->setStateInformation (state0.getData(), static_cast<int> (state0.getSize()));
            nameInstance (*duplicate, "Duplicate", std::nullopt);
        }

        int created = 0, reportedAndGone = 0;
        std::vector<std::unique_ptr<AudioPluginInstance>> medium;
        AudioBuffer<float> scratch (2, o.blockSize);
        MidiBuffer scratchMidi;
        const auto churnEnd = 24.0;

        while (elapsed() < churnEnd)
        {
            // Short-lived (like a host's plugin scan): must never be reported.
            auto shortLived = createInstance (*format, desc, o.rate, o.blockSize, error);
            if (shortLived != nullptr)
            {
                ++created;
                nameInstance (*shortLived, "Scan", std::nullopt);
                for (int b = 0; b < 4; ++b)
                    shortLived->processBlock (scratch, scratchMidi);
                shortLived->releaseResources();
                shortLived.reset();
            }

            // Medium-lived: reported, then removed -> must say bye.
            if (created % 10 == 0)
            {
                auto m = createInstance (*format, desc, o.rate, o.blockSize, error);
                if (m != nullptr)
                {
                    nameInstance (*m, "Medium", std::nullopt);
                    medium.push_back (std::move (m));
                }
            }
            if (medium.size() > 3)
                medium.erase (medium.begin());

            pump (40);
        }
        medium.clear();
        pump (500);
        phase ("17-24.5 s churn (instances +/-)");

        const auto snapAfter = companion.snapshot();
        int scansSeen = 0, mediumSeen = 0;
        for (const auto& [id, inst] : snapAfter)
        {
            scansSeen += inst.name == "Scan" ? 1 : 0;
            if (inst.name == "Medium")
            {
                ++mediumSeen;
                reportedAndGone += inst.gone ? 1 : 0;
            }
        }
        check ("scan-like instances (alive < 1 s) never reach the companion", scansSeen == 0,
               std::to_string (created) + " created and destroyed, " + std::to_string (scansSeen) + " reported");
        check ("removed instances say bye", mediumSeen > 0 && reportedAndGone == mediumSeen,
               std::to_string (reportedAndGone) + "/" + std::to_string (mediumSeen) + " reported ones marked gone");

        const auto dupId = companion.idForName ("Duplicate");
        check ("a duplicated instance (state copied, id included) reports a NEW id", dupOk && dupId != 0 && dupId != id0,
               "original " + idToHex (id0) + ", duplicate " + idToHex (dupId));
        auto* dupVolume = dupOk ? findParameter (*duplicate, "Volume") : nullptr;
        check ("...while keeping the copied settings", dupVolume != nullptr
                   && std::abs (dupVolume->getValue() - static_cast<float> (volume14ToFaderPosition (volume14ForDb (-12.0)))) < 1e-4f);
        if (duplicate != nullptr)
        {
            duplicate->releaseResources();
            duplicate.reset();
        }
        check ("original still reports its own id after the duplicate left", companion.idForName ("Track 000") == id0);
    }

    // --- run to the end ------------------------------------------------------
    while (audio.isThreadRunning())
        pump (20);

    if (o.companion && o.realtime && ! o.steady)
        phase ("24.5-30 s steady");

    const double wallMs = Time::getMillisecondCounterHiRes() - wallStart;
    const double busCpuMs = threadCpuMsNamed ("Mixer SEND bus") - busCpuBefore;

    for (auto* inst : list)
        inst->removeListener (&spy);

    // --- teardown: every instance released, the bus thread must go -----------
    for (auto& inst : owned)
        inst->releaseResources();
    owned.clear();
    list.clear();
    pump (100);
    check ("bus thread gone once the last instance is destroyed", countThreadsNamed ("Mixer SEND bus") == 0);
    companion.stop();

    // --- report ----------------------------------------------------------------
    const auto blocks = audio.blocksDone.load();
    std::vector<double> times (audio.blockTimesNs.begin(), audio.blockTimesNs.begin() + blocks);
    const auto s = summarise (times);
    const double budgetUs = o.blockSize / o.rate * 1.0e6;
    const double audioSeconds = blocks * o.blockSize / o.rate;

    std::printf ("\n  audio thread: %d blocks = %.1f s of audio in %.1f s wall, %d deadline overruns\n",
                 blocks, audioSeconds, wallMs / 1000.0, audio.overruns.load());
    std::printf ("  per block, all %d instances: mean %.1f us  p50 %.1f  p99 %.1f  p99.9 %.1f  max %.1f us  (budget %.0f us)\n",
                 o.instances, s.mean / 1000.0, s.p50 / 1000.0, s.p99 / 1000.0, s.p999 / 1000.0, s.max / 1000.0, budgetUs);
    std::printf ("  per instance per block: mean %.0f ns, p99 %.0f ns  -> %.3f %% of one core for %d instances\n",
                 s.mean / o.instances, s.p99 / o.instances, 100.0 * s.mean / 1000.0 / budgetUs, o.instances);
    std::printf ("  audio thread CPU time %.0f ms (%.2f %% of wall)\n", audio.cpuNs / 1.0e6, 100.0 * audio.cpuNs / 1.0e6 / wallMs);
    std::printf ("  bus thread CPU time %.1f ms over %.1f s (%.3f %% of one core)\n", busCpuMs, wallMs / 1000.0, 100.0 * busCpuMs / wallMs);
    const auto totalAllocations = audioThreadAllocations.load();
    const auto laterAllocations = totalAllocations - audio.firstBlockAllocations;
    std::printf ("  heap allocations on the audio thread inside the plugins: %llu in the first block (this thread's\n"
                 "    one-time thread-local block), %llu in the %d blocks after it (self-test saw %llu)\n",
                 static_cast<unsigned long long> (audio.firstBlockAllocations), static_cast<unsigned long long> (laterAllocations),
                 blocks - 1, static_cast<unsigned long long> (audio.selfTestAllocations));
    if (o.companion)
        std::printf ("  fake companion: %llu hellos, %llu info, %llu state datagrams, %llu byes, %llu rejected\n",
                     (unsigned long long) companion.hellos.load(), (unsigned long long) companion.infos.load(),
                     (unsigned long long) companion.stateDatagrams.load(), (unsigned long long) companion.byes.load(),
                     (unsigned long long) companion.rejected.load());

    printRecordedStacks (audioThreadAllocations.load());
    check ("allocation counter works (self-test)", audio.selfTestAllocations >= 1);
    check ("first block: at most the one-time thread-local block per binary", audio.firstBlockAllocations <= 1,
           std::to_string (audio.firstBlockAllocations) + " allocation(s)");
    check ("no heap allocation on the audio thread after its first block", laterAllocations == 0,
           std::to_string (laterAllocations) + " allocations");
    check ("ran the full length", blocks == totalBlocks, std::to_string (blocks) + "/" + std::to_string (totalBlocks) + " blocks");

    malloc_logger = nullptr;

    int failed = 0;
    for (const auto& c : checks)
        failed += c.pass ? 0 : 1;

    std::printf ("\n%s: %d/%zu checks passed\n", failed == 0 ? "PASS" : "FAIL", static_cast<int> (checks.size()) - failed, checks.size());
    return failed == 0 ? 0 : 1;
}
