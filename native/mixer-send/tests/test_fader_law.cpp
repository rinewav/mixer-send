// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "test_framework.h"

#include "core/FaderLaw.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <limits>

using namespace mixersend;

namespace
{
    juce::var loadFaderFixture()
    {
        const juce::File file (juce::String (test::fixturePath ("fader-law-golden.json")));
        return juce::JSON::parse (file.loadFileAsString());
    }
}

TEST (fader_law_matches_the_shared_golden_positions)
{
    const auto fixture = loadFaderFixture();
    EXPECT (fixture.isObject());

    const double tol = fixture["tolerance"]["db"];
    const auto& points = *fixture["points"].getArray();
    EXPECT (points.size() == static_cast<int> (std::size (kFaderLawPoints)));

    for (int i = 0; i < points.size(); ++i)
    {
        EXPECT_NEAR (static_cast<double> (points[i][0]), kFaderLawPoints[i].position, 0.0);
        EXPECT_NEAR (static_cast<double> (points[i][1]), kFaderLawPoints[i].decibels, 0.0);
    }

    for (const auto& row : *fixture["positionToDb"].getArray())
    {
        const double position = row["position"];
        const double db = faderPositionToDecibels (position);

        if (row["db"].isVoid())
            EXPECT (std::isinf (db) && db < 0.0);
        else
            EXPECT_NEAR (db, static_cast<double> (row["db"]), tol);
    }

    for (const auto& row : *fixture["volume14ToDb"].getArray())
    {
        const int v14 = row["volume14"];
        const double db = faderPositionToDecibels (volume14ToFaderPosition (v14));

        if (row["db"].isVoid())
            EXPECT (std::isinf (db) && db < 0.0);
        else
            EXPECT_NEAR (db, static_cast<double> (row["db"]), tol);
    }

    const double ptol = fixture["tolerance"]["position"];
    for (const auto& row : *fixture["dbToPosition"].getArray())
        EXPECT_NEAR (decibelsToFaderPosition (row["db"]), static_cast<double> (row["position"]), ptol);
}

TEST (unity_is_exactly_one_at_the_default_and_at_the_surface_unity_value)
{
    EXPECT (faderPositionToGain (0.76f) == 1.0f);            // the parameter default (float)
    EXPECT (faderPositionToGain (kUnityPosition) == 1.0f);
    EXPECT (faderPositionToGain (volume14ToFaderPosition (12451)) == 1.0f); // Studio Pro's unity
    EXPECT (faderPositionToVolume14 (0.76f) == 12451);
    EXPECT (faderPositionToGain (0.0) == 0.0f);
    EXPECT_NEAR (faderPositionToGain (1.0), std::pow (10.0, 10.0 / 20.0), 1e-6);
}

TEST (fader_law_is_monotonic_and_invertible)
{
    double previous = -std::numeric_limits<double>::infinity();

    for (int i = 1; i <= 16383; ++i)
    {
        const double position = volume14ToFaderPosition (i);
        const double db = faderPositionToDecibels (position);
        EXPECT (db > previous);
        previous = db;
        EXPECT_NEAR (decibelsToFaderPosition (db), position, 1e-9);
        EXPECT (faderPositionToVolume14 (position) == i);
    }
}
