// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin/Parameters.h"

#include "core/FaderLaw.h"

#include <cmath>

namespace mixersend::params
{

juce::String volumeToText (float position)
{
    const double decibels = faderPositionToDecibels (position);

    if (std::isinf (decibels) || decibels < -99.95)
        return "-inf";

    // Match what the fader actually does: the unity snap reads as 0.0.
    const double shown = std::abs (decibels) < kUnitySnapDecibels ? 0.0 : decibels;
    return (shown > 0.04 ? "+" : "") + juce::String (shown, 1);
}

std::optional<float> volumeFromText (const juce::String& text)
{
    const auto trimmed = text.trim().toLowerCase().upToFirstOccurrenceOf ("db", false, true).trim();

    if (trimmed.isEmpty())
        return std::nullopt;

    if (trimmed == "-inf" || trimmed == "-infinity" || trimmed == "inf" || trimmed == "off")
        return 0.0f;

    if (! trimmed.containsOnly ("+-0123456789.,"))
        return std::nullopt;

    const double decibels = trimmed.replaceCharacter (',', '.').getDoubleValue();
    return static_cast<float> (juce::jlimit (0.0, 1.0, decibelsToFaderPosition (decibels)));
}

juce::String panToText (float pan)
{
    const int rounded = juce::roundToInt (pan);

    if (rounded == 0)
        return "C";

    return (rounded < 0 ? "L" : "R") + juce::String (std::abs (rounded));
}

std::optional<float> panFromText (const juce::String& text)
{
    const auto t = text.trim().toUpperCase();

    if (t.isEmpty())
        return std::nullopt;

    if (t == "C" || t == "CENTER" || t == "CENTRE")
        return 0.0f;

    const auto number = t.retainCharacters ("0123456789.");

    if (number.isEmpty())
        return std::nullopt;

    const float magnitude = juce::jlimit (0.0f, 100.0f, number.getFloatValue());

    if (t.startsWithChar ('L') || t.startsWithChar ('-'))
        return -magnitude;

    return magnitude;
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { volumeId, 1 },
        "Volume",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        static_cast<float> (kUnityPosition),
        juce::AudioParameterFloatAttributes()
            .withLabel ("dB")
            .withStringFromValueFunction ([] (float value, int) { return volumeToText (value); })
            .withValueFromStringFunction ([] (const juce::String& text) {
                return volumeFromText (text).value_or (static_cast<float> (kUnityPosition));
            })));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { panId, 1 },
        "Pan",
        juce::NormalisableRange<float> (-100.0f, 100.0f, 1.0f),
        0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float value, int) { return panToText (value); })
            .withValueFromStringFunction ([] (const juce::String& text) { return panFromText (text).value_or (0.0f); })));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { muteId, 1 },
        "Mute",
        false));

    return layout;
}

} // namespace mixersend::params
