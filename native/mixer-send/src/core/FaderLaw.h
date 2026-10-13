// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace mixersend
{

/*  The touch fader law, identical to the one the surface uses for Studio Pro
    (prototypes/mixer-concepts/SPEC.md "Fader law", measured on Studio Pro
    8.1.1 and carried as kTouchTaper by the device script). A fader position
    therefore means the same level in Mixer SEND mode as in Studio Pro mode.

    - position -> dB is piecewise linear through kFaderLawPoints;
    - below the first point (0.1226, -46.02 dB) the GAIN falls linearly to
      zero at position 0 (so dB = -46.02 + 20 log10(position / 0.1226));
    - position 0 is -inf, unity (0 dB) is 0.76, the top (1.0) is +10 dB.

    Pinned against packages/protocol/fixtures/fader-law-golden.json, the same
    file the companion's TypeScript copy is tested against. */

struct FaderLawPoint
{
    double position;
    double decibels;
};

inline constexpr FaderLawPoint kFaderLawPoints[] = {
    { 0.1226, -46.02 }, { 0.1594, -40.0 }, { 0.2317, -33.98 }, { 0.2742, -30.46 },
    { 0.3276, -26.02 }, { 0.3767, -21.94 }, { 0.4188, -18.42 }, { 0.4612, -14.89 },
    { 0.4955, -12.04 }, { 0.5411, -9.12 },  { 0.5689, -7.96 },  { 0.5935, -6.94 },
    { 0.6155, -6.02 },  { 0.6535, -4.44 },  { 0.6856, -3.1 },   { 0.7134, -1.94 },
    { 0.738, -0.92 },   { 0.76, 0.0 },      { 0.7798, 0.83 },   { 0.798, 1.58 },
    { 0.858, 4.08 },    { 0.9045, 6.02 },   { 1.0, 10.0 },
};

/** 14-bit fader scale shared with the surface (faderValue14). */
inline constexpr int kVolume14Max = 16383;

/** Where 0 dB sits on the fader. */
inline constexpr double kUnityPosition = 0.76;

/** Levels within this distance of 0 dB are applied as EXACT unity gain. One
    14-bit step near unity is about 0.0026 dB, so the surface's own unity value
    (12451, -0.0002 dB) and its neighbours land on a bit-transparent 1.0 instead
    of a gain that is inaudibly but measurably off. */
inline constexpr double kUnitySnapDecibels = 0.005;

/** dB for a fader position in [0, 1]; -infinity at 0 and below. */
double faderPositionToDecibels (double position) noexcept;

/** Inverse of faderPositionToDecibels. -inf -> 0, above +10 dB -> 1. */
double decibelsToFaderPosition (double decibels) noexcept;

/** Linear gain for a position, with the unity snap applied. 0 at position 0. */
float faderPositionToGain (double position) noexcept;

/** Position <-> 14-bit scale (round to nearest, clamped). */
int faderPositionToVolume14 (double position) noexcept;
double volume14ToFaderPosition (int volume14) noexcept;

} // namespace mixersend
