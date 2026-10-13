// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

/*
    Mixer SEND mini host.

    A tiny, portable VST3 host for smoke tests where the full harness
    (tools/host_harness.cpp, macOS-only: mach timing, malloc_logger) does not
    build -- in particular the Windows build, run under wine on the build Mac.

    It loads the built bundle through JUCE's VST3 hosting (the path a DAW
    takes), gives each instance a track name and colour, processes a -12 dBFS
    sine on a paced audio thread while the main thread runs the message loop
    (so remote edits from a companion reach the parameters), then prints what
    the instances ended up with and checks that state saves and restores.

    usage:
      mixer_send_mini_host --plugin <path.vst3> [--instances 2] [--seconds 8]
                           [--rate 48000] [--block 256]

    Prints one line per fact; exit 0 = loaded, processed audio, state round-trip.
*/

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace juce;

namespace
{
    struct Options
    {
        String plugin;
        int instances = 2;
        double seconds = 8.0;
        double rate = 48000.0;
        int block = 256;
    };

    Options parse (int argc, char** argv)
    {
        Options o;
        for (int i = 1; i < argc; ++i)
        {
            const String a (argv[i]);
            auto next = [&] { return i + 1 < argc ? String (argv[++i]) : String(); };
            if (a == "--plugin") o.plugin = next();
            else if (a == "--instances") o.instances = jlimit (1, 256, next().getIntValue());
            else if (a == "--seconds") o.seconds = jlimit (0.5, 3600.0, next().getDoubleValue());
            else if (a == "--rate") o.rate = next().getDoubleValue();
            else if (a == "--block") o.block = jlimit (16, 4096, next().getIntValue());
        }
        return o;
    }

    AudioProcessorParameter* findParameter (AudioPluginInstance& instance, const String& name)
    {
        for (auto* p : instance.getParameters())
            if (p->getName (64).equalsIgnoreCase (name))
                return p;
        return nullptr;
    }

    String describe (AudioPluginInstance& instance)
    {
        String text;
        for (auto* p : instance.getParameters())
            text << p->getName (32) << "=" << p->getCurrentValueAsText() << "  ";
        return text.trimEnd();
    }
}

int main (int argc, char** argv)
{
    const auto o = parse (argc, argv);

    if (o.plugin.isEmpty())
    {
        std::fprintf (stderr, "usage: mixer_send_mini_host --plugin <path.vst3> [--instances n] [--seconds s]\n");
        return 2;
    }

    ScopedJuceInitialiser_GUI juceInit;

    VST3PluginFormat format;
    OwnedArray<PluginDescription> found;
    format.findAllTypesForFile (found, o.plugin);

    if (found.isEmpty())
    {
        std::printf ("FAIL no VST3 class found in %s\n", o.plugin.toRawUTF8());
        return 1;
    }

    const auto desc = *found[0];
    std::printf ("plugin    %s by %s, version %s, %s, uid %08x, %d in / %d out\n", desc.name.toRawUTF8(),
                 desc.manufacturerName.toRawUTF8(), desc.version.toRawUTF8(), desc.pluginFormatName.toRawUTF8(),
                 static_cast<unsigned> (desc.uniqueId), desc.numInputChannels, desc.numOutputChannels);

    std::vector<std::unique_ptr<AudioPluginInstance>> instances;

    for (int i = 0; i < o.instances; ++i)
    {
        String error;
        auto instance = format.createInstanceFromDescription (desc, o.rate, o.block, error);

        if (instance == nullptr)
        {
            std::printf ("FAIL instance %d: %s\n", i, error.toRawUTF8());
            return 1;
        }

        instance->enableAllBuses();
        instance->setPlayConfigDetails (2, 2, o.rate, o.block);
        instance->prepareToPlay (o.rate, o.block);

        AudioProcessor::TrackProperties track;
        track.name = "Mini Host " + String (i + 1);
        track.colourARGB = 0xff2a9d8fu + static_cast<uint32> (i) * 0x00101000u;
        instance->updateTrackProperties (track);

        instances.push_back (std::move (instance));
    }

    std::printf ("instances %d created, %s\n", o.instances, describe (*instances.front()).toRawUTF8());

    // --- audio: a paced thread, like a DAW's --------------------------------
    std::atomic<bool> running { true };
    std::atomic<int64> blocks { 0 };
    std::vector<float> peaks (instances.size(), 0.0f);

    std::thread audio ([&]
    {
        AudioBuffer<float> buffer (2, o.block);
        MidiBuffer midi;
        double phase = 0.0;
        const double step = MathConstants<double>::twoPi * 440.0 / o.rate;
        const auto blockDuration = std::chrono::duration<double> (o.block / o.rate);
        auto next = std::chrono::steady_clock::now();

        while (running.load())
        {
            for (size_t n = 0; n < instances.size(); ++n)
            {
                for (int s = 0; s < o.block; ++s)
                {
                    const auto v = static_cast<float> (0.25 * std::sin (phase + step * s)); // -12 dBFS
                    buffer.setSample (0, s, v);
                    buffer.setSample (1, s, v);
                }
                instances[n]->processBlock (buffer, midi);
                peaks[n] = buffer.getMagnitude (0, o.block);
            }
            phase = std::fmod (phase + step * o.block, MathConstants<double>::twoPi);
            ++blocks;
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration> (blockDuration);
            std::this_thread::sleep_until (next);
        }
    });

    // --- message loop: remote edits arrive here (AsyncUpdater) -------------
    MessageManager::getInstance()->runDispatchLoopUntil (static_cast<int> (o.seconds * 1000.0));

    running = false;
    audio.join();

    std::printf ("audio     %lld blocks of %d at %.0f Hz\n", static_cast<long long> (blocks.load()), o.block, o.rate);

    for (size_t n = 0; n < instances.size(); ++n)
        std::printf ("instance  %d: %s  output peak %.2f dBFS\n", static_cast<int> (n + 1),
                     describe (*instances[n]).toRawUTF8(), Decibels::gainToDecibels (peaks[n], -100.0f));

    // --- state round trip ---------------------------------------------------
    MemoryBlock state;
    instances.front()->getStateInformation (state);
    bool restored = false;
    {
        String error;
        auto copy = format.createInstanceFromDescription (desc, o.rate, o.block, error);
        if (copy != nullptr)
        {
            copy->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
            auto* a = findParameter (*instances.front(), "Volume");
            auto* b = findParameter (*copy, "Volume");
            restored = a != nullptr && b != nullptr && std::abs (a->getValue() - b->getValue()) < 1.0e-6f;
        }
    }
    std::printf ("state     %d bytes, restored into a new instance: %s\n", static_cast<int> (state.getSize()),
                 restored ? "same volume" : "MISMATCH");

    for (auto& instance : instances)
        instance->releaseResources();
    instances.clear();

    const bool ok = blocks.load() > 0 && restored;
    std::printf ("%s\n", ok ? "MINI HOST OK" : "MINI HOST FAILED");
    return ok ? 0 : 1;
}
