// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/FaderLaw.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace mixersend
{

double faderPositionToDecibels (double position) noexcept
{
    if (! (position > 0.0)) // also catches NaN
        return -std::numeric_limits<double>::infinity();

    const auto& first = kFaderLawPoints[0];

    if (position < first.position)
        return first.decibels + 20.0 * std::log10 (position / first.position);

    for (std::size_t i = 0; i + 1 < std::size (kFaderLawPoints); ++i)
    {
        const auto& a = kFaderLawPoints[i];
        const auto& b = kFaderLawPoints[i + 1];

        if (position <= b.position)
            return a.decibels + (position - a.position) / (b.position - a.position) * (b.decibels - a.decibels);
    }

    return std::rbegin (kFaderLawPoints)->decibels;
}

double decibelsToFaderPosition (double decibels) noexcept
{
    if (std::isnan (decibels) || decibels == -std::numeric_limits<double>::infinity())
        return 0.0;

    const auto& first = kFaderLawPoints[0];

    if (decibels < first.decibels)
        return first.position * std::pow (10.0, (decibels - first.decibels) / 20.0);

    for (std::size_t i = 0; i + 1 < std::size (kFaderLawPoints); ++i)
    {
        const auto& a = kFaderLawPoints[i];
        const auto& b = kFaderLawPoints[i + 1];

        if (decibels <= b.decibels)
            return a.position + (decibels - a.decibels) / (b.decibels - a.decibels) * (b.position - a.position);
    }

    return 1.0;
}

float faderPositionToGain (double position) noexcept
{
    const double decibels = faderPositionToDecibels (position);

    if (std::isinf (decibels))
        return 0.0f;

    if (std::abs (decibels) < kUnitySnapDecibels)
        return 1.0f;

    return static_cast<float> (std::pow (10.0, decibels / 20.0));
}

int faderPositionToVolume14 (double position) noexcept
{
    if (! (position > 0.0))
        return 0;

    return static_cast<int> (std::lround (std::min (position, 1.0) * kVolume14Max));
}

double volume14ToFaderPosition (int volume14) noexcept
{
    return static_cast<double> (std::clamp (volume14, 0, kVolume14Max)) / kVolume14Max;
}

} // namespace mixersend
