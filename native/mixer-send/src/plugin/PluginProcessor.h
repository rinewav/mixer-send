// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "bus/InstanceLink.h"
#include "bus/SendBus.h"
#include "core/GainStage.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace mixersend
{

/*  One Mixer SEND instance: gain + balance + mute, 0 latency, and a link to
    the process-wide bus that reports it to the companion.

    The audio thread only reads atomics (three parameters and the lease flag)
    and writes atomics (meters): no allocation, no locks, no system calls, no
    logging. Remote changes from the companion arrive on the bus thread, are
    parked in the link's mailbox and applied HERE on the message thread with
    begin/setValueNotifyingHost/end, so the host records them like any other
    edit and nothing ever writes a parameter from the audio thread. */
class MixerSendProcessor final : public juce::AudioProcessor,
                                 private juce::AsyncUpdater,
                                 private juce::Timer
{
public:
    MixerSendProcessor();
    ~MixerSendProcessor() override;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void updateTrackProperties (const TrackProperties& properties) override;
    void numChannelsChanged() override;

    // --- for the editor and tools ----------------------------------------
    juce::AudioProcessorValueTreeState& state() noexcept { return apvts; }
    const InstanceLink& link() const noexcept { return instanceLink; }
    bool isCompanionConnected() const noexcept { return bus != nullptr && bus->isConnected(); }
    std::uint64_t instanceId() const noexcept { return instanceLink.id.load(); }

private:
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void applyRemote (int index, float normalisedValue);
    void updateTargets (int numChannels, bool jump) noexcept;
    void publishLevels (const juce::AudioBuffer<float>& buffer, int numChannels) noexcept;

    juce::AudioProcessorValueTreeState apvts;
    juce::RangedAudioParameter* parameters[3] {}; // volume, pan, mute
    std::atomic<float>* volumePosition = nullptr;
    std::atomic<float>* pan = nullptr;
    std::atomic<float>* mute = nullptr;

    InstanceLink instanceLink;
    std::shared_ptr<SendBus> bus;

    // --- audio thread ----------------------------------------------------
    GainStage stage;
    float lastVolume = -1.0f, lastPan = 0.0f;
    bool lastMuted = false;
    int lastChannels = -1;

    // --- message thread: remote edits held open as one host gesture ------
    bool gestureOpen[3] {};
    juce::uint32 lastRemoteEditMs[3] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerSendProcessor)
};

} // namespace mixersend
