// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "test_framework.h"

#include "core/FaderLaw.h"
#include "core/GainStage.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace mixersend;

namespace
{
    struct StereoBuffer
    {
        std::vector<float> left, right;
        float* channels[2];

        explicit StereoBuffer (int n, float value = 1.0f) : left (static_cast<std::size_t> (n), value), right (static_cast<std::size_t> (n), value)
        {
            channels[0] = left.data();
            channels[1] = right.data();
        }
    };

    std::vector<float> noise (int n, unsigned seed)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
        std::vector<float> v (static_cast<std::size_t> (n));
        for (auto& x : v)
            x = dist (rng);
        return v;
    }
}

TEST (unity_centre_is_bit_transparent)
{
    GainStage stage;
    stage.prepare (48000.0);
    stage.jumpTo (targetGains (kUnityPosition, 0.0f, false, 2));

    const auto l = noise (64, 1), r = noise (64, 2);
    auto bl = l, br = r;
    float* ch[2] = { bl.data(), br.data() };
    stage.process (ch, 2, 64);

    EXPECT (bl == l);
    EXPECT (br == r);
}

TEST (balance_law_keeps_the_near_side_and_attenuates_the_far_side_linearly)
{
    EXPECT (balanceGains (0.0f) == (ChannelGains { 1.0f, 1.0f }));
    EXPECT (balanceGains (-100.0f) == (ChannelGains { 1.0f, 0.0f }));
    EXPECT (balanceGains (100.0f) == (ChannelGains { 0.0f, 1.0f }));
    EXPECT_NEAR (balanceGains (50.0f).left, 0.5, 1e-7);
    EXPECT (balanceGains (50.0f).right == 1.0f);
    EXPECT_NEAR (balanceGains (-25.0f).right, 0.75, 1e-7);
    EXPECT (balanceGains (-25.0f).left == 1.0f);
    EXPECT (balanceGains (1000.0f) == (ChannelGains { 0.0f, 1.0f })); // clamped
    EXPECT (balanceGains (std::nanf ("")) == (ChannelGains { 1.0f, 1.0f }));

    // Level and pan compose multiplicatively.
    const auto g = targetGains (decibelsToFaderPosition (-6.0), 50.0f, false, 2);
    EXPECT_NEAR (g.right, std::pow (10.0, -6.0 / 20.0), 1e-5);
    EXPECT_NEAR (g.left, 0.5 * std::pow (10.0, -6.0 / 20.0), 1e-5);
}

TEST (hard_pan_on_a_running_stage_reaches_exact_values)
{
    GainStage stage;
    stage.prepare (48000.0);
    stage.jumpTo ({ 1.0f, 1.0f });
    stage.setTarget (targetGains (kUnityPosition, -100.0f, false, 2));

    for (int block = 0; block < 20; ++block)
    {
        StereoBuffer b (64);
        stage.process (b.channels, 2, 64);
    }

    StereoBuffer b (64);
    stage.process (b.channels, 2, 64);
    EXPECT (std::all_of (b.left.begin(), b.left.end(), [] (float x) { return x == 1.0f; }));
    EXPECT (std::all_of (b.right.begin(), b.right.end(), [] (float x) { return x == 0.0f; }));
}

TEST (mute_ramps_to_exact_silence_in_twenty_ms)
{
    GainStage stage;
    stage.prepare (48000.0);
    EXPECT (stage.rampLengthSamples() == 960);
    stage.jumpTo (targetGains (kUnityPosition, 0.0f, false, 2));
    stage.setTarget (targetGains (kUnityPosition, 0.0f, true, 2));

    StereoBuffer b (960);
    stage.process (b.channels, 2, 960);
    EXPECT (b.left.front() < 1.0f && b.left.front() > 0.99f); // first sample already moving, but barely
    EXPECT (b.left.back() == 0.0f);
    EXPECT (! stage.isRamping());

    StereoBuffer after (64, 0.7f);
    stage.process (after.channels, 2, 64);
    EXPECT (std::all_of (after.left.begin(), after.left.end(), [] (float x) { return x == 0.0f; }));
    EXPECT (std::all_of (after.right.begin(), after.right.end(), [] (float x) { return x == 0.0f; }));
}

TEST (smoothing_step_is_bounded_under_random_remote_changes)
{
    // DC input of 1 makes the output the gain itself, sample by sample.
    GainStage stage;
    stage.prepare (48000.0);
    stage.jumpTo ({ 1.0f, 1.0f });

    std::mt19937 rng (7);
    std::uniform_real_distribution<float> position (0.0f, 1.0f), panDist (-100.0f, 100.0f);

    const float maxGain = faderPositionToGain (1.0);
    const float bound = maxGain / static_cast<float> (stage.rampLengthSamples()) * 1.0001f;
    float previous[2] = { 1.0f, 1.0f };
    float worst = 0.0f;

    for (int block = 0; block < 4000; ++block)
    {
        // A new target on most blocks: worse than any real drag.
        if (block % 3 != 0)
            stage.setTarget (targetGains (position (rng), panDist (rng), rng() % 17 == 0, 2));

        StereoBuffer b (64);
        stage.process (b.channels, 2, 64);

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto& v = ch == 0 ? b.left : b.right;
            for (const float x : v)
            {
                worst = std::max (worst, std::abs (x - previous[ch]));
                previous[ch] = x;
            }
        }
    }

    EXPECT_MSG (worst <= bound, "worst step " + std::to_string (worst) + " bound " + std::to_string (bound));
}

TEST (ramp_length_follows_the_sample_rate)
{
    GainStage stage;
    stage.prepare (44100.0);
    EXPECT (stage.rampLengthSamples() == 882);
    stage.prepare (96000.0);
    EXPECT (stage.rampLengthSamples() == 1920);
    stage.prepare (0.0); // nonsense rate: a sane default, never zero
    EXPECT (stage.rampLengthSamples() > 0);
}

TEST (mono_path_applies_gain_and_ignores_pan)
{
    const auto g = targetGains (decibelsToFaderPosition (-12.0), -100.0f, false, 1);
    EXPECT_NEAR (g.left, std::pow (10.0, -12.0 / 20.0), 1e-5);

    GainStage stage;
    stage.prepare (48000.0);
    stage.jumpTo (g);

    std::vector<float> mono (64, 0.5f);
    float* ch[1] = { mono.data() };
    stage.process (ch, 1, 64);

    for (const float x : mono)
        EXPECT_NEAR (x, 0.5 * std::pow (10.0, -12.0 / 20.0), 1e-6);

    EXPECT (targetGains (kUnityPosition, 0.0f, true, 1) == (ChannelGains { 0.0f, 0.0f }));
}

TEST (minus_infinity_clears_the_block)
{
    GainStage stage;
    stage.prepare (48000.0);
    stage.jumpTo (targetGains (0.0, 0.0f, false, 2));

    StereoBuffer b (64, 0.9f);
    stage.process (b.channels, 2, 64);
    EXPECT (std::all_of (b.left.begin(), b.left.end(), [] (float x) { return x == 0.0f; }));
}

TEST (levels_and_meter_handoff_report_every_block_since_the_last_read)
{
    std::vector<float> a (64, 0.25f), bneg (64, -0.5f);
    const float* ch[2] = { a.data(), bneg.data() };
    const auto levels = measureLevels (ch, 2, 64);
    EXPECT_NEAR (levels.peak[0], 0.25, 1e-7);
    EXPECT_NEAR (levels.peak[1], 0.5, 1e-7);
    EXPECT_NEAR (levels.sumSquares[0], 64 * 0.0625, 1e-5);

    MeterHandoff meters;
    BlockLevels loud, quiet;
    loud.peak[0] = 0.8f; loud.peak[1] = 0.4f; loud.sumSquares[0] = 64 * 0.25f; loud.sumSquares[1] = 0.0f;
    quiet.peak[0] = 0.1f; quiet.peak[1] = 0.1f; quiet.sumSquares[0] = 0.0f; quiet.sumSquares[1] = 0.0f;

    meters.push (loud, 2, 64);
    meters.push (quiet, 2, 64);
    auto reading = meters.take();
    EXPECT_NEAR (reading.peak[0], 0.8, 1e-7);      // the loud block is not forgotten
    EXPECT_NEAR (reading.rms[0], std::sqrt (0.125), 1e-6); // mean over both blocks
    EXPECT (reading.blocks == 2);

    meters.push (quiet, 2, 64); // a fresh window after the read
    reading = meters.take();
    EXPECT_NEAR (reading.peak[0], 0.1, 1e-7);
    EXPECT (reading.blocks == 3);

    // Mono reports the one channel on both sides.
    MeterHandoff mono;
    mono.push (loud, 1, 64);
    reading = mono.take();
    EXPECT (reading.peak[0] == reading.peak[1]);
}

TEST (non_finite_audio_never_poisons_the_meter)
{
    std::vector<float> bad (64, 0.1f);
    bad[10] = std::nanf ("");
    const float* ch[1] = { bad.data() };
    const auto levels = measureLevels (ch, 1, 64);
    EXPECT (std::isfinite (levels.peak[0]) && std::isfinite (levels.sumSquares[0]));
}

TEST (bench_dsp_cost_per_block)
{
    // What processBlock does per block, minus the plugin-format call around
    // it: gain stage (settled, or ramping), levels, meter handoff.
    GainStage stage;
    stage.prepare (48000.0);
    MeterHandoff meters;
    StereoBuffer b (64, 0.5f);
    constexpr int blocks = 200000;

    const auto run = [&] (bool ramping) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < blocks; ++i)
        {
            if (ramping && i % 16 == 0)
                stage.setTarget (targetGains ((i % 32) == 0 ? 0.5 : 0.7, 10.0f, false, 2));
            stage.process (b.channels, 2, 64);
            const float* read[2] = { b.channels[0], b.channels[1] };
            meters.push (measureLevels (read, 2, 64), 2, 64);
        }
        return std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - start).count() / blocks;
    };

    stage.jumpTo ({ 1.0f, 1.0f });
    const double unity = run (false);
    stage.jumpTo (targetGains (0.5, 20.0f, false, 2));
    const double settled = run (false);
    const double ramping = run (true);
    std::printf ("      DSP per 64-sample stereo block: unity %.0f ns, settled gain %.0f ns, ramping %.0f ns\n",
                 unity, settled, ramping);
    EXPECT (unity < 2000.0 && settled < 2000.0 && ramping < 2000.0);
}
