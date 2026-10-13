// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "plugin/PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>
#include <string>

namespace mixersend
{

/** Flat, vector-only look: nothing cached as images, so it is as sharp at
    200% as at 100% and costs nothing while idle. */
class MixerSendLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    MixerSendLookAndFeel();

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                           float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPosProportional,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;
    int getSliderThumbRadius (juce::Slider&) override;

    static float faderUnit (const juce::Slider&) noexcept;

    juce::Colour accent = juce::Colour (0xff4fb3ff);
};

/*  Volume (a fader with a dB readout), pan, mute, and one status line: is the
    companion linked, and which track the host says this is.

    Repaints come only from the controls themselves and from a 15 Hz poll of
    the status that repaints the header ONLY when something in it changed --
    no animation, no meters, nothing that costs CPU while a project sits
    there with a hundred of these open. */
class MixerSendEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    static constexpr int baseWidth = 300;
    static constexpr int baseHeight = 236;

    explicit MixerSendEditor (MixerSendProcessor&);
    ~MixerSendEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Status
    {
        bool connected = false;
        bool leaseMuted = false;
        std::string trackName;
        std::optional<std::uint32_t> colour;

        bool operator== (const Status&) const = default;
    };

    Status readStatus() const;
    void timerCallback() override;
    void showLicense();
    float scale() const noexcept { return static_cast<float> (getWidth()) / static_cast<float> (baseWidth); }

    MixerSendProcessor& owner;
    MixerSendLookAndFeel lookAndFeel;

    juce::Slider volume { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::Slider pan { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::TextButton muteButton { "MUTE" };
    juce::Label volumeCaption, panCaption;
    juce::HyperlinkButton licenseLink { "AGPLv3", juce::URL() };
    juce::ScopedMessageBox licenseBox;

    juce::AudioProcessorValueTreeState::SliderAttachment volumeAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment panAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment muteAttachment;

    Status shown;
    juce::Rectangle<int> headerArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerSendEditor)
};

} // namespace mixersend
