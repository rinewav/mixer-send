// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin/PluginProcessor.h"

#include "core/FaderLaw.h"
#include "core/InstanceId.h"
#include "plugin/Parameters.h"
#include "plugin/PluginEditor.h"

namespace mixersend
{

namespace
{
    enum ParameterIndex
    {
        volumeIndex = 0,
        panIndex = 1,
        muteIndex = 2,
    };

    /** How long a burst of remote edits is held open as ONE host gesture, so
        a drag on the surface records like a drag, not like 25 clicks. */
    constexpr juce::uint32 gestureIdleMs = 250;

    /*  JUCE's VST3 wrapper keeps a `thread_local` (inParameterChangedCallback)
        that it first touches when the host delivers a parameter change inside
        process(). dyld creates a binary's thread-local block lazily, with a
        small calloc, the first time a thread touches any of its variables --
        so without this, that one allocation would land on the audio thread
        mid-playback, at the first automation or remote edit. Touching our own
        thread_local at the top of every block moves it to each audio thread's
        very first call into this binary, and it is the only allocation the
        audio thread ever sees (measured: tools/host_harness.cpp). */
    thread_local volatile bool threadLocalBlockReady = true;

    inline void touchThreadLocalBlock() noexcept
    {
        [[maybe_unused]] const bool ready = threadLocalBlockReady;
    }

    wire::Format formatFor (juce::AudioProcessor::WrapperType type) noexcept
    {
        using P = juce::AudioProcessor;

        if (type == P::wrapperType_VST3) return wire::Format::vst3;
        if (type == P::wrapperType_AudioUnit) return wire::Format::au;
        if (type == P::wrapperType_AudioUnitv3) return wire::Format::auv3;
        if (type == P::wrapperType_Standalone) return wire::Format::standalone;
        return wire::Format::unknown;
    }

    std::string describeHost()
    {
        juce::PluginHostType host;
        juce::String name (host.getHostDescription());

        if (name.isEmpty() || name == "Unknown")
            name = juce::File (juce::PluginHostType::getHostPath()).getFileNameWithoutExtension();

        return name.toStdString();
    }

    SendBus::Identity busIdentity (juce::AudioProcessor::WrapperType type)
    {
        SendBus::Identity identity;
        identity.format = formatFor (type);
        identity.hostName = describeHost();
        identity.pluginVersion = (MIXER_SEND_VERSION_MAJOR << 16) | (MIXER_SEND_VERSION_MINOR << 8) | MIXER_SEND_VERSION_PATCH;
        return identity;
    }
}

//==============================================================================
MixerSendProcessor::MixerSendProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "MixerSend", params::createLayout())
{
    parameters[volumeIndex] = apvts.getParameter (params::volumeId);
    parameters[panIndex] = apvts.getParameter (params::panId);
    parameters[muteIndex] = apvts.getParameter (params::muteId);
    volumePosition = apvts.getRawParameterValue (params::volumeId);
    pan = apvts.getRawParameterValue (params::panId);
    mute = apvts.getRawParameterValue (params::muteId);

    instanceLink.id.store (randomNonZeroId());
    instanceLink.volumePosition = volumePosition;
    instanceLink.pan = pan;
    instanceLink.mute = mute;
    instanceLink.numChannels.store (juce::jmax (1, getTotalNumInputChannels()));
    instanceLink.onRemoteChange = [this] { triggerAsyncUpdate(); };

    bus = SendBus::acquire (busIdentity (wrapperType));
    bus->registerInstance (instanceLink);
}

MixerSendProcessor::~MixerSendProcessor()
{
    // FIRST: after this the bus thread can no longer reach this instance, its
    // parameters or its callback (see InstanceLink).
    if (bus != nullptr)
        bus->unregisterInstance (instanceLink);

    cancelPendingUpdate();
    stopTimer();
}

//==============================================================================
bool MixerSendProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (in != out || in.isDisabled())
        return false;

    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void MixerSendProcessor::numChannelsChanged()
{
    instanceLink.numChannels.store (juce::jmax (1, getTotalNumInputChannels()));
    instanceLink.infoDirty.store (true);

    if (bus != nullptr)
        bus->wake();
}

void MixerSendProcessor::prepareToPlay (double sampleRate, int)
{
    stage.prepare (sampleRate);

    // Start AT the current target, not ramping to it from unity: an instance
    // restored at -inf must not open with a 20 ms burst of full level.
    updateTargets (juce::jlimit (1, 2, getTotalNumInputChannels()), true);
    numChannelsChanged();
}

void MixerSendProcessor::updateTargets (int numChannels, bool jump) noexcept
{
    const float volume = volumePosition->load (std::memory_order_relaxed);
    const float panValue = pan->load (std::memory_order_relaxed);
    const bool muted = mute->load (std::memory_order_relaxed) > 0.5f
                    || instanceLink.leaseMuted.load (std::memory_order_acquire);

    if (! jump && juce::exactlyEqual (volume, lastVolume) && juce::exactlyEqual (panValue, lastPan) && muted == lastMuted && numChannels == lastChannels)
        return;

    lastVolume = volume;
    lastPan = panValue;
    lastMuted = muted;
    lastChannels = numChannels;

    const auto target = targetGains (volume, panValue, muted, numChannels);

    if (jump)
        stage.jumpTo (target);
    else
        stage.setTarget (target);
}

void MixerSendProcessor::publishLevels (const juce::AudioBuffer<float>& buffer, int numChannels) noexcept
{
    const auto levels = measureLevels (buffer.getArrayOfReadPointers(), numChannels, buffer.getNumSamples());
    instanceLink.meters.push (levels, numChannels, buffer.getNumSamples());
}

void MixerSendProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    touchThreadLocalBlock();
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels(), 2);

    if (numSamples <= 0 || numChannels <= 0)
        return;

    // After bypass lastVolume is -1, so this always sets a fresh target and
    // the stage ramps to it from unity (where bypass left it), never jumps.
    updateTargets (numChannels, false);
    stage.process (buffer.getArrayOfWritePointers(), numChannels, numSamples);
    publishLevels (buffer, numChannels);
    instanceLink.bypassed.store (false, std::memory_order_relaxed);
}

void MixerSendProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    touchThreadLocalBlock();
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels(), 2);

    if (numSamples <= 0 || numChannels <= 0)
        return;

    // Bypass = unity pass-through, reached by the same 20 ms ramp so that
    // toggling bypass on a turned-down channel does not click either.
    stage.setTarget ({ 1.0f, 1.0f });
    lastVolume = -1.0f; // forces a fresh target after bypass
    stage.process (buffer.getArrayOfWritePointers(), numChannels, numSamples);
    publishLevels (buffer, numChannels);
    instanceLink.bypassed.store (true, std::memory_order_relaxed);
}

//==============================================================================
void MixerSendProcessor::handleAsyncUpdate()
{
    const auto change = instanceLink.takeRemoteChange();

    if (change.mask == 0)
        return;

    if ((change.mask & wire::controlMask::volume) != 0)
        applyRemote (volumeIndex, static_cast<float> (volume14ToFaderPosition (change.volume14)));

    if ((change.mask & wire::controlMask::pan) != 0)
        applyRemote (panIndex, parameters[panIndex]->convertTo0to1 (static_cast<float> (change.pan)));

    if ((change.mask & wire::controlMask::mute) != 0)
        applyRemote (muteIndex, change.muted ? 1.0f : 0.0f);

    instanceLink.ackSeq.store (change.seq, std::memory_order_release);
}

void MixerSendProcessor::applyRemote (int index, float normalisedValue)
{
    auto* parameter = parameters[index];

    if (parameter == nullptr)
        return;

    if (! gestureOpen[index])
    {
        parameter->beginChangeGesture();
        gestureOpen[index] = true;
    }

    if (! juce::exactlyEqual (parameter->getValue(), normalisedValue))
        parameter->setValueNotifyingHost (normalisedValue);

    lastRemoteEditMs[index] = juce::Time::getMillisecondCounter();

    if (! isTimerRunning())
        startTimer (50);
}

void MixerSendProcessor::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    bool anyOpen = false;

    for (int i = 0; i < 3; ++i)
    {
        if (! gestureOpen[i])
            continue;

        if (now - lastRemoteEditMs[i] >= gestureIdleMs)
        {
            parameters[i]->endChangeGesture();
            gestureOpen[i] = false;
        }
        else
        {
            anyOpen = true;
        }
    }

    if (! anyOpen)
        stopTimer();
}

//==============================================================================
void MixerSendProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("instanceId", juce::String (idToHex (instanceLink.id.load())), nullptr);
    state.setProperty ("stateVersion", 1, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MixerSendProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    const auto requested = idFromHex (tree.getProperty ("instanceId").toString().toStdString());
    tree.removeProperty ("instanceId", nullptr);
    tree.removeProperty ("stateVersion", nullptr);

    apvts.replaceState (tree);

    // A duplicated instance restores its original's id; the bus refuses a
    // second live holder in this process and this instance keeps a fresh one.
    if (requested.has_value() && bus != nullptr)
        bus->claimId (instanceLink, *requested);
    else if (requested.has_value())
        instanceLink.id.store (*requested);
}

void MixerSendProcessor::updateTrackProperties (const TrackProperties& properties)
{
    std::optional<std::string> name;

    if (properties.name.has_value() && properties.name->trim().isNotEmpty())
        name = properties.name->toStdString();

    instanceLink.setTrackProperties (name.has_value() ? std::optional<std::string_view> (*name) : std::nullopt,
                                     properties.colourARGB);

    if (bus != nullptr)
        bus->wake();
}

juce::AudioProcessorEditor* MixerSendProcessor::createEditor()
{
    return new MixerSendEditor (*this);
}

} // namespace mixersend

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new mixersend::MixerSendProcessor();
}
