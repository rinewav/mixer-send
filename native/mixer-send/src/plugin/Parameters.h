// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>

namespace mixersend::params
{

inline constexpr const char* volumeId = "volume";
inline constexpr const char* panId = "pan";
inline constexpr const char* muteId = "mute";

/*  Volume is stored as FADER POSITION (0..1), not as dB or gain: host
    automation then moves exactly like the touch fader, and the position maps
    1:1 onto the surface's 14-bit scale. Its text is dB through the shared
    fader law, so every host shows and accepts dB. Default 0.76 = 0 dB. */
juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

juce::String volumeToText (float position);
std::optional<float> volumeFromText (const juce::String& text);

juce::String panToText (float pan);
std::optional<float> panFromText (const juce::String& text);

} // namespace mixersend::params
