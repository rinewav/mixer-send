// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin/PluginEditor.h"

#include "core/FaderLaw.h"
#include "plugin/Parameters.h"

namespace mixersend
{

namespace
{
    namespace palette
    {
        const juce::Colour background { 0xff15171b };
        const juce::Colour panel { 0xff1e2228 };
        const juce::Colour groove { 0xff2c323b };
        const juce::Colour text { 0xffe8eaed };
        const juce::Colour dim { 0xff8a919b };
        const juce::Colour faint { 0xff5a616b };
        const juce::Colour cap { 0xffd9dde3 };
        const juce::Colour linked { 0xff3ecf8e };
        const juce::Colour offline { 0xff6b7280 };
        const juce::Colour mute { 0xffe5484d };
        const juce::Colour lease { 0xfff5a524 };
    }

    juce::Font uiFont (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
    }

    constexpr float faderMarks[] = { 10.0f, 5.0f, 0.0f, -5.0f, -10.0f, -20.0f, -30.0f, -40.0f, -60.0f };

    constexpr auto sourceUrl = "https://github.com/rinewav/mixer-send";

    // The AGPLv3 "Appropriate Legal Notices" (section 5d): copyright, no warranty, the
    // licence, and where to get it and the source.
    juce::String licenseNotice()
    {
        return juce::String ("Copyright 2026 Covo Design\n\n"
                             "Mixer SEND is free software: you can redistribute it and/or modify it under the "
                             "terms of the GNU Affero General Public License, version 3, as published by the "
                             "Free Software Foundation. It comes with ABSOLUTELY NO WARRANTY.\n\n"
                             "Built with JUCE (AGPLv3) and the VST 3 SDK. VST is a registered trademark of "
                             "Steinberg Media Technologies GmbH. This software is based in part on the work "
                             "of the Independent JPEG Group.\n\n"
                             "The licence, the notices of the code it includes, and the complete source code: ")
               + sourceUrl;
    }
}

//==============================================================================
MixerSendLookAndFeel::MixerSendLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, palette::text);
    setColour (juce::Slider::textBoxBackgroundColourId, palette::panel);
    setColour (juce::Slider::textBoxOutlineColourId, palette::groove);
    setColour (juce::Slider::textBoxHighlightColourId, palette::groove);
    setColour (juce::Label::textColourId, palette::dim);
    setColour (juce::TextEditor::backgroundColourId, palette::panel);
    setColour (juce::TextEditor::textColourId, palette::text);
    setColour (juce::TextEditor::highlightColourId, palette::groove);
    setColour (juce::TextButton::textColourOffId, palette::dim);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::CaretComponent::caretColourId, palette::text);
}

float MixerSendLookAndFeel::faderUnit (const juce::Slider& slider) noexcept
{
    return juce::jmax (0.75f, static_cast<float> (slider.getWidth()) / 138.0f);
}

int MixerSendLookAndFeel::getSliderThumbRadius (juce::Slider& slider)
{
    if (slider.getSliderStyle() == juce::Slider::LinearVertical)
        return juce::roundToInt (7.5f * faderUnit (slider));

    return LookAndFeel_V4::getSliderThumbRadius (slider);
}

juce::Font MixerSendLookAndFeel::getLabelFont (juce::Label& label)
{
    return uiFont (juce::jmax (9.0f, static_cast<float> (label.getHeight()) * 0.62f));
}

juce::Font MixerSendLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return uiFont (juce::jmax (9.0f, static_cast<float> (buttonHeight) * 0.42f), true);
}

void MixerSendLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                             float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                             juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    juce::ignoreUnused (minSliderPos, maxSliderPos); // two-value sliders only

    // JUCE maps the value onto exactly this rectangle (already inset by the
    // thumb radius): value 1 at the top edge, value 0 at the bottom edge.
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const float unit = faderUnit (slider);
    const float grooveWidth = 4.0f * unit;
    const float centreX = bounds.getX() + bounds.getWidth() * 0.62f;
    const float top = bounds.getY();
    const float bottom = bounds.getBottom();

    // Scale: where the law actually puts each mark, so the ticks agree with
    // the surface's own fader scale.
    g.setFont (uiFont (8.5f * unit));
    for (const float mark : faderMarks)
    {
        const float position = static_cast<float> (decibelsToFaderPosition (mark));
        const float markY = bottom + (top - bottom) * position;
        const bool unity = mark == 0.0f;
        g.setColour (unity ? palette::dim : palette::faint);
        g.drawHorizontalLine (juce::roundToInt (markY), centreX - grooveWidth * 3.0f, centreX - grooveWidth * 1.2f);

        const juce::String label = mark > 0.0f ? "+" + juce::String (juce::roundToInt (mark)) : juce::String (juce::roundToInt (mark));
        g.drawText (label, juce::Rectangle<float> (bounds.getX(), markY - 5.0f * unit, centreX - grooveWidth * 3.4f - bounds.getX(), 10.0f * unit),
                    juce::Justification::centredRight, false);
    }

    g.setColour (palette::groove);
    g.fillRoundedRectangle (centreX - grooveWidth * 0.5f, top, grooveWidth, bottom - top, grooveWidth * 0.5f);

    g.setColour (accent.withAlpha (slider.isEnabled() ? 0.85f : 0.3f));
    g.fillRoundedRectangle (centreX - grooveWidth * 0.5f, sliderPos, grooveWidth, juce::jmax (0.0f, bottom - sliderPos), grooveWidth * 0.5f);

    const float capWidth = juce::jmin (bounds.getWidth() * 0.42f, 30.0f * unit);
    const float capHeight = 13.0f * unit;
    const auto capBounds = juce::Rectangle<float> (capWidth, capHeight).withCentre ({ centreX, sliderPos });
    g.setColour (palette::cap);
    g.fillRoundedRectangle (capBounds, 2.5f * unit);
    g.setColour (palette::background);
    g.fillRect (capBounds.withSizeKeepingCentre (capWidth * 0.8f, juce::jmax (1.0f, unit)));
}

void MixerSendLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                             float proportion, float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 3.0f;
    const auto centre = bounds.getCentre();
    const float thickness = juce::jmax (2.0f, radius * 0.16f);
    const float angle = startAngle + proportion * (endAngle - startAngle);
    const float middle = (startAngle + endAngle) * 0.5f;

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, endAngle, true);
    g.setColour (palette::groove);
    g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Bipolar: the value arc grows from the centre (C), not from hard left.
    if (std::abs (angle - middle) > 0.001f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, juce::jmin (middle, angle), juce::jmax (middle, angle), true);
        g.setColour (accent.withAlpha (slider.isEnabled() ? 0.9f : 0.3f));
        g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const float knobRadius = radius - thickness * 1.6f;
    g.setColour (palette::panel.brighter (0.15f));
    g.fillEllipse (juce::Rectangle<float> (knobRadius * 2.0f, knobRadius * 2.0f).withCentre (centre));

    const auto tip = centre.getPointOnCircumference (knobRadius * 0.82f, angle);
    const auto base = centre.getPointOnCircumference (knobRadius * 0.25f, angle);
    g.setColour (palette::text);
    g.drawLine ({ base, tip }, juce::jmax (1.5f, thickness * 0.5f));
}

void MixerSendLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                                 bool isMouseOverButton, bool isButtonDown)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const float corner = bounds.getHeight() * 0.22f;
    const bool on = button.getToggleState();

    auto fill = on ? palette::mute : palette::panel;
    if (isButtonDown)
        fill = fill.darker (0.2f);
    else if (isMouseOverButton)
        fill = fill.brighter (0.08f);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, corner);
    g.setColour (on ? palette::mute.brighter (0.2f) : palette::groove);
    g.drawRoundedRectangle (bounds, corner, 1.0f);
}

//==============================================================================
MixerSendEditor::MixerSendEditor (MixerSendProcessor& p)
    : AudioProcessorEditor (p),
      owner (p),
      volumeAttachment (p.state(), params::volumeId, volume),
      panAttachment (p.state(), params::panId, pan),
      muteAttachment (p.state(), params::muteId, muteButton)
{
    setLookAndFeel (&lookAndFeel);

    volume.setTextValueSuffix (" dB");
    volume.setDoubleClickReturnValue (true, kUnityPosition);
    volume.setTitle ("Volume");
    volume.setTooltip ("Volume. Double-click for 0 dB.");
    addAndMakeVisible (volume);

    pan.setRotaryParameters (juce::degreesToRadians (-135.0f) + juce::MathConstants<float>::twoPi,
                             juce::degreesToRadians (135.0f) + juce::MathConstants<float>::twoPi, true);
    pan.setDoubleClickReturnValue (true, 0.0);
    pan.setTitle ("Pan");
    addAndMakeVisible (pan);

    muteButton.setClickingTogglesState (true);
    muteButton.setTitle ("Mute");
    addAndMakeVisible (muteButton);

    for (auto* caption : { &volumeCaption, &panCaption })
    {
        caption->setJustificationType (juce::Justification::centred);
        caption->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (caption);
    }
    volumeCaption.setText ("VOLUME", juce::dontSendNotification);
    panCaption.setText ("PAN", juce::dontSendNotification);

    licenseLink.setTitle ("Licence");
    licenseLink.setColour (juce::HyperlinkButton::textColourId, palette::faint);
    licenseLink.onClick = [this] { showLicense(); };
    addAndMakeVisible (licenseLink);

    shown = readStatus();
    if (shown.colour.has_value())
        lookAndFeel.accent = juce::Colour (*shown.colour).withAlpha (1.0f);

    setResizable (true, true);
    setResizeLimits (baseWidth * 3 / 4, baseHeight * 3 / 4, baseWidth * 5 / 2, baseHeight * 5 / 2);
    getConstrainer()->setFixedAspectRatio (static_cast<double> (baseWidth) / baseHeight);
    setSize (baseWidth, baseHeight);

    startTimerHz (15);
}

MixerSendEditor::~MixerSendEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void MixerSendEditor::showLicense()
{
    const auto options = juce::MessageBoxOptions()
                             .withIconType (juce::MessageBoxIconType::InfoIcon)
                             .withTitle ("Mixer SEND " JucePlugin_VersionString)
                             .withMessage (licenseNotice())
                             .withButton ("Open the source page")
                             .withButton ("Close")
                             .withAssociatedComponent (this);

    // Two buttons: the first answers 1, the second 0.
    licenseBox = juce::AlertWindow::showScopedAsync (options, [] (int result)
    {
        if (result == 1)
            juce::URL (sourceUrl).launchInDefaultBrowser();
    });
}

MixerSendEditor::Status MixerSendEditor::readStatus() const
{
    Status status;
    status.connected = owner.isCompanionConnected();
    status.leaseMuted = owner.link().leaseMuted.load (std::memory_order_relaxed);
    status.trackName = std::string (owner.link().trackNameCopy().view());
    status.colour = owner.link().trackColour();
    return status;
}

void MixerSendEditor::timerCallback()
{
    const auto now = readStatus();

    if (now == shown)
        return;

    const bool colourChanged = now.colour != shown.colour;
    shown = now;

    if (colourChanged)
    {
        lookAndFeel.accent = shown.colour.has_value() ? juce::Colour (*shown.colour).withAlpha (1.0f)
                                                      : juce::Colour (0xff4fb3ff);
        repaint();
        return;
    }

    repaint (headerArea);
}

void MixerSendEditor::paint (juce::Graphics& g)
{
    const float s = scale();
    g.fillAll (palette::background);

    const auto header = headerArea.toFloat();

    if (shown.colour.has_value())
    {
        g.setColour (juce::Colour (*shown.colour).withAlpha (1.0f));
        g.fillRect (0.0f, 0.0f, 5.0f * s, static_cast<float> (getHeight()));
    }

    // Title
    g.setColour (palette::dim);
    g.setFont (uiFont (11.0f * s, true));
    g.drawText ("MIXER SEND", juce::Rectangle<float> (16.0f * s, 10.0f * s, 120.0f * s, 16.0f * s),
                juce::Justification::centredLeft, false);

    // Link status: a dot and one or two words, right-aligned.
    juce::String statusText;
    juce::Colour statusColour;

    if (shown.leaseMuted)
    {
        statusText = "Muted by solo";
        statusColour = palette::lease;
    }
    else if (shown.connected)
    {
        statusText = "Linked";
        statusColour = palette::linked;
    }
    else
    {
        statusText = "Touch Console offline";
        statusColour = palette::offline;
    }

    const auto statusFont = uiFont (10.5f * s);
    g.setFont (statusFont);
    const float textWidth = juce::GlyphArrangement::getStringWidth (statusFont, statusText) + 2.0f;
    const float right = header.getRight() - 14.0f * s;
    g.setColour (shown.connected || shown.leaseMuted ? palette::text : palette::dim);
    g.drawText (statusText, juce::Rectangle<float> (right - textWidth, 10.0f * s, textWidth, 16.0f * s),
                juce::Justification::centredRight, false);
    g.setColour (statusColour);
    const float dot = 7.0f * s;
    g.fillEllipse (right - textWidth - dot - 5.0f * s, 18.0f * s - dot * 0.5f, dot, dot);

    // Track name, or an honest note that the host gave none.
    const auto nameArea = juce::Rectangle<float> (16.0f * s, 30.0f * s, header.getWidth() - 30.0f * s, 22.0f * s);
    if (shown.trackName.empty())
    {
        g.setColour (palette::faint);
        g.setFont (uiFont (12.0f * s));
        g.drawText ("Track name not provided by host", nameArea, juce::Justification::centredLeft, true);
    }
    else
    {
        g.setColour (palette::text);
        g.setFont (uiFont (16.0f * s, true));
        g.drawText (juce::String::fromUTF8 (shown.trackName.c_str()), nameArea, juce::Justification::centredLeft, true);
    }

    g.setColour (palette::groove);
    g.fillRect (16.0f * s, header.getBottom() - 1.0f, header.getWidth() - 30.0f * s, 1.0f);
}

void MixerSendEditor::resized()
{
    const float s = scale();
    const auto px = [s] (float v) { return juce::roundToInt (v * s); };

    auto bounds = getLocalBounds();
    headerArea = bounds.removeFromTop (px (60));
    // Beside the title "MIXER SEND" (16..~76 px at 1x), clear of the right-aligned status.
    licenseLink.setFont (uiFont (10.0f * s), false, juce::Justification::centredLeft);
    licenseLink.setBounds (px (84), px (10), px (44), px (16));
    bounds = bounds.withTrimmedLeft (px (12)).withTrimmedRight (px (12)).withTrimmedTop (px (8)).withTrimmedBottom (px (10));

    auto left = bounds.removeFromLeft (bounds.getWidth() / 2);
    auto right = bounds;

    volumeCaption.setBounds (left.removeFromTop (px (14)));
    volume.setTextBoxStyle (juce::Slider::TextBoxBelow, false, px (78), px (22));
    volume.setBounds (left);

    panCaption.setBounds (right.removeFromTop (px (14)));
    auto muteArea = right.removeFromBottom (px (30));
    muteButton.setBounds (muteArea.withSizeKeepingCentre (px (76), px (26)));
    right.removeFromBottom (px (8));
    pan.setTextBoxStyle (juce::Slider::TextBoxBelow, false, px (56), px (22));
    pan.setBounds (right);
}

} // namespace mixersend
