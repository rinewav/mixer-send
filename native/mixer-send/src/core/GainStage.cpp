// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/GainStage.h"

#include "core/FaderLaw.h"

#include <algorithm>
#include <cmath>

#if defined(__APPLE__)
 #include <Accelerate/Accelerate.h>
#endif

namespace mixersend
{

ChannelGains balanceGains (float pan) noexcept
{
    const float p = std::clamp (std::isfinite (pan) ? pan / 100.0f : 0.0f, -1.0f, 1.0f);
    return { p > 0.0f ? 1.0f - p : 1.0f, p < 0.0f ? 1.0f + p : 1.0f };
}

ChannelGains targetGains (double faderPosition, float pan, bool muted, int numChannels) noexcept
{
    if (muted)
        return { 0.0f, 0.0f };

    const float gain = faderPositionToGain (faderPosition);

    if (numChannels < 2)
        return { gain, gain };

    const auto balance = balanceGains (pan);
    return { gain * balance.left, gain * balance.right };
}

//==============================================================================
void GainStage::prepare (double sampleRate, double rampSeconds) noexcept
{
    const double samples = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate * rampSeconds : 960.0;
    rampSamples = std::max (1, static_cast<int> (std::lround (samples)));

    // A ramp in flight was sized for the old rate: finish it now rather than
    // let it run at the wrong speed.
    for (auto& ramp : ramps)
    {
        ramp.current = ramp.target;
        ramp.remaining = 0;
        ramp.step = 0.0f;
    }
}

void GainStage::startRamp (Ramp& ramp, float newTarget) const noexcept
{
    if (newTarget == ramp.target)
        return;

    ramp.target = newTarget;
    ramp.remaining = rampSamples;
    ramp.step = (newTarget - ramp.current) / static_cast<float> (rampSamples);
}

void GainStage::setTarget (ChannelGains newTarget) noexcept
{
    startRamp (ramps[0], newTarget.left);
    startRamp (ramps[1], newTarget.right);
}

void GainStage::jumpTo (ChannelGains gains) noexcept
{
    ramps[0] = { gains.left, gains.left, 0.0f, 0 };
    ramps[1] = { gains.right, gains.right, 0.0f, 0 };
}

void GainStage::processChannel (Ramp& ramp, float* samples, int numSamples) noexcept
{
    int i = 0;

    if (ramp.remaining > 0)
    {
        const int rampPart = std::min (ramp.remaining, numSamples);
        const bool finishes = rampPart == ramp.remaining;
        const float start = ramp.current;
        const float step = ramp.step;

        // Closed form rather than a running sum: no loop-carried dependency,
        // so the compiler can vectorise it, and no accumulated rounding. The
        // ramp's last sample uses the target itself, so a fade to silence
        // ends on an exact 0 and a return to unity on an exact 1.
        const int interpolated = finishes ? rampPart - 1 : rampPart;

        for (; i < interpolated; ++i)
            samples[i] *= start + step * static_cast<float> (i + 1);

        if (finishes)
            samples[i++] *= ramp.target;

        ramp.remaining -= rampPart;
        ramp.current = ramp.remaining == 0 ? ramp.target : start + step * static_cast<float> (rampPart);
    }

    if (i >= numSamples)
        return;

    const float gain = ramp.current;

    if (gain == 1.0f)
        return;

    if (gain == 0.0f)
    {
        std::fill (samples + i, samples + numSamples, 0.0f);
        return;
    }

    for (; i < numSamples; ++i)
        samples[i] *= gain;
}

void GainStage::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    if (channels == nullptr || numSamples <= 0)
        return;

    for (int ch = 0; ch < std::min (numChannels, 2); ++ch)
        if (channels[ch] != nullptr)
            processChannel (ramps[ch], channels[ch], numSamples);

    // A mono stage keeps its unused right ramp in step, so a later switch to
    // stereo does not start from a stale value.
    if (numChannels == 1)
        ramps[1] = ramps[0];
}

//==============================================================================
BlockLevels measureLevels (const float* const* channels, int numChannels, int numSamples) noexcept
{
    BlockLevels levels;

    if (channels == nullptr || numSamples <= 0)
        return levels;

    for (int ch = 0; ch < std::min (numChannels, 2); ++ch)
    {
        const float* x = channels[ch];

        if (x == nullptr)
            continue;

       #if defined(__APPLE__)
        const auto n = static_cast<vDSP_Length> (numSamples);
        vDSP_maxmgv (x, 1, &levels.peak[ch], n);
        vDSP_svesq (x, 1, &levels.sumSquares[ch], n);
       #else
        float peak = 0.0f, sum = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            peak = std::max (peak, std::abs (x[i]));
            sum += x[i] * x[i];
        }
        levels.peak[ch] = peak;
        levels.sumSquares[ch] = sum;
       #endif

        // A NaN or inf in the stream must not poison the meter forever.
        if (! std::isfinite (levels.peak[ch]) || ! std::isfinite (levels.sumSquares[ch]))
        {
            levels.peak[ch] = 0.0f;
            levels.sumSquares[ch] = 0.0f;
        }
    }

    return levels;
}

//==============================================================================
void MeterHandoff::push (const BlockLevels& levels, int numChannels, int numSamples) noexcept
{
    const auto consumedNow = consumed.load (std::memory_order_acquire);

    // A window grows only while nobody reads it; cap it (about 20 s at 48 kHz)
    // so a bus that stopped reading cannot overflow the sample count.
    if (consumedNow != seenConsumed || windowSamples > (1u << 20))
    {
        seenConsumed = consumedNow;
        windowSamples = 0;
        windowPeak[0] = windowPeak[1] = 0.0f;
        windowSum[0] = windowSum[1] = 0.0;
    }

    const int channelsToRead = numChannels >= 2 ? 2 : 1;

    for (int ch = 0; ch < channelsToRead; ++ch)
    {
        windowPeak[ch] = std::max (windowPeak[ch], levels.peak[ch]);
        windowSum[ch] += static_cast<double> (levels.sumSquares[ch]);
    }

    if (channelsToRead == 1)
    {
        windowPeak[1] = windowPeak[0];
        windowSum[1] = windowSum[0];
    }

    windowSamples += static_cast<std::uint32_t> (std::max (numSamples, 0));

    const double denominator = windowSamples > 0 ? static_cast<double> (windowSamples) : 1.0;

    for (int ch = 0; ch < 2; ++ch)
    {
        publishedPeak[ch].store (windowPeak[ch], std::memory_order_relaxed);
        publishedMeanSquare[ch].store (static_cast<float> (windowSum[ch] / denominator), std::memory_order_relaxed);
    }

    blocks.store (++blockCount, std::memory_order_release);
}

MeterHandoff::Reading MeterHandoff::take() noexcept
{
    Reading reading;
    reading.blocks = blocks.load (std::memory_order_acquire);

    for (int ch = 0; ch < 2; ++ch)
    {
        reading.peak[ch] = publishedPeak[ch].load (std::memory_order_relaxed);
        reading.rms[ch] = std::sqrt (std::max (0.0f, publishedMeanSquare[ch].load (std::memory_order_relaxed)));
    }

    consumed.fetch_add (1, std::memory_order_release);
    return reading;
}

} // namespace mixersend
