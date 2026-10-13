// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstdint>

namespace mixersend
{

/** Per-channel linear gains for one block. Mono uses `left` only. */
struct ChannelGains
{
    float left = 1.0f;
    float right = 1.0f;

    bool operator== (const ChannelGains&) const = default;
};

/*  Balance law for a stereo pair (pan in -100..+100).

    The side being panned TOWARDS stays at unity; the other side falls
    linearly in gain to silence at the extreme:

        p = pan / 100
        left  = p > 0 ? 1 - p : 1
        right = p < 0 ? 1 + p : 1

    Centre is unity on both sides (a stereo track at pan 0 is untouched, bit
    for bit), hard left keeps the left channel exactly and removes the right,
    and half-way (pan +-50) leaves the far side at -6.02 dB. There is no
    centre attenuation and no cross-feed: this is a balance control for a
    stereo signal, not a mono panner. */
ChannelGains balanceGains (float pan) noexcept;

/** What the stage should converge to for the given parameter values. */
ChannelGains targetGains (double faderPosition, float pan, bool muted, int numChannels) noexcept;

/*  Gain application with a linear ramp, so every change -- a host automation
    step, a touch fader drag, a mute -- moves the gain over `rampSeconds`
    (20 ms by default) instead of stepping it. A new target mid-ramp starts a
    fresh ramp from wherever the gain is, so the per-sample change is bounded
    by |target - current| / rampLength and can never click.

    Real-time safe: no allocation, no locks, no system calls. When the gain is
    settled at exactly 1 the audio is not touched at all; at exactly 0 it is
    cleared; otherwise one vectorisable multiply. */
class GainStage
{
public:
    void prepare (double sampleRate, double rampSeconds = 0.020) noexcept;

    /** Start ramping towards `target` (no-op if it is already the target). */
    void setTarget (ChannelGains target) noexcept;

    /** Jump without a ramp (only for initialisation). */
    void jumpTo (ChannelGains gains) noexcept;

    /** Process `numChannels` (1 or 2) channels in place. */
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    ChannelGains current() const noexcept { return { ramps[0].current, ramps[1].current }; }
    ChannelGains target() const noexcept { return { ramps[0].target, ramps[1].target }; }
    bool isRamping() const noexcept { return ramps[0].remaining > 0 || ramps[1].remaining > 0; }
    int rampLengthSamples() const noexcept { return rampSamples; }

private:
    struct Ramp
    {
        float current = 1.0f;
        float target = 1.0f;
        float step = 0.0f;
        int remaining = 0;
    };

    static void processChannel (Ramp& ramp, float* samples, int numSamples) noexcept;
    void startRamp (Ramp& ramp, float target) const noexcept;

    Ramp ramps[2];
    int rampSamples = 960;
};

/** Peak magnitude and sum of squares of one block, per channel. */
struct BlockLevels
{
    float peak[2] {};
    float sumSquares[2] {};
};

BlockLevels measureLevels (const float* const* channels, int numChannels, int numSamples) noexcept;

/*  Hands level readings from the audio thread to the bus thread without a
    lock and without a read-modify-write shared by two writers.

    The audio thread is the ONLY writer of the published levels; the bus
    thread is the only writer of `consumed`. After reading, the bus bumps
    `consumed`; the audio thread notices at its next block and starts a fresh
    accumulation window. So each reading is the peak and the RMS over every
    block since the previous reading (at worst one block that lands exactly
    between the bus's read and its bump is counted in neither window, which a
    meter cannot show). */
class MeterHandoff
{
public:
    struct Reading
    {
        float peak[2] {};
        float rms[2] {};
        std::uint32_t blocks = 0; ///< audio blocks processed so far (wraps)
    };

    // --- audio thread -----------------------------------------------------
    void push (const BlockLevels& levels, int numChannels, int numSamples) noexcept;

    // --- bus thread -------------------------------------------------------
    Reading take() noexcept;

private:
    std::atomic<float> publishedPeak[2] {};
    std::atomic<float> publishedMeanSquare[2] {};
    std::atomic<std::uint32_t> blocks { 0 };
    std::atomic<std::uint32_t> consumed { 0 };

    // audio-thread private
    std::uint32_t seenConsumed = 0;
    std::uint32_t blockCount = 0;
    std::uint32_t windowSamples = 0;
    float windowPeak[2] {};
    double windowSum[2] {};
};

} // namespace mixersend
