#!/usr/bin/env python3
"""Golden values for the touch fader law (prototypes/mixer-concepts/SPEC.md).

The law is the one measured on Studio Pro 8.1.1 (`kTouchTaper` in
devices/studio-pro-touch/TouchConsoleComponent.js): normalised position -> dB,
piecewise linear through the measured points; below the lowest point the GAIN
falls linearly to zero at position 0. Mixer SEND applies this law to audio, and
the companion uses it to label positions, so both are tested against this file:

    python3 packages/protocol/fixtures/gen_fader_law_golden.py \
        > packages/protocol/fixtures/fader-law-golden.json
    pnpm exec biome format --write packages/protocol/fixtures/fader-law-golden.json
"""

import json
import math

POINTS = [
    (0.1226, -46.02), (0.1594, -40.0), (0.2317, -33.98), (0.2742, -30.46), (0.3276, -26.02),
    (0.3767, -21.94), (0.4188, -18.42), (0.4612, -14.89), (0.4955, -12.04), (0.5411, -9.12),
    (0.5689, -7.96), (0.5935, -6.94), (0.6155, -6.02), (0.6535, -4.44), (0.6856, -3.1),
    (0.7134, -1.94), (0.738, -0.92), (0.76, 0.0), (0.7798, 0.83), (0.798, 1.58),
    (0.858, 4.08), (0.9045, 6.02), (1.0, 10.0),
]


def position_to_db(position):
    """dB for a position; None stands for -inf (position 0)."""
    if position <= 0.0:
        return None
    first_pos, first_db = POINTS[0]
    if position < first_pos:
        return first_db + 20.0 * math.log10(position / first_pos)
    for (p0, d0), (p1, d1) in zip(POINTS, POINTS[1:]):
        if position <= p1:
            return d0 + (position - p0) / (p1 - p0) * (d1 - d0)
    return POINTS[-1][1]


def db_to_position(db):
    """Inverse of position_to_db. -inf (None) -> 0; above +10 dB -> 1."""
    if db is None:
        return 0.0
    first_pos, first_db = POINTS[0]
    if db < first_db:
        return first_pos * 10.0 ** ((db - first_db) / 20.0)
    for (p0, d0), (p1, d1) in zip(POINTS, POINTS[1:]):
        if db <= d1:
            return p0 + (db - d0) / (d1 - d0) * (p1 - p0)
    return 1.0


positions = [0.0, 0.001, 0.05, 0.1, 0.1226, 0.14, 0.25, 0.5, 0.6155, 0.7, 0.75, 0.76, 0.77,
             0.8, 0.858, 0.9, 0.9045, 0.95, 1.0]
volume14 = [0, 1, 100, 2008, 2009, 4096, 8192, 10000, 12450, 12451, 12452, 14000, 16383]
decibels = [-120.0, -60.0, -46.02, -40.0, -20.0, -12.0, -6.02, -3.0, -0.5, 0.0, 0.5, 3.0, 6.02, 10.0]


def db_json(value):
    return None if value is None else round(value, 9)


fixture = {
    "law": "touch-fader",
    "description": (
        "Touch fader law (SPEC.md 'Fader law'): position -> dB piecewise linear through "
        "'points'; below the first point the gain falls linearly to 0 at position 0 "
        "(dB = firstDb + 20*log10(position/firstPosition)); position 0 is -inf, written null. "
        "volume14 positions are value / 16383. Unity (0 dB) is position 0.76."
    ),
    "points": [[p, d] for p, d in POINTS],
    "fader14Max": 16383,
    "positionToDb": [{"position": p, "db": db_json(position_to_db(p))} for p in positions],
    "volume14ToDb": [{"volume14": v, "db": db_json(position_to_db(v / 16383.0))} for v in volume14],
    "dbToPosition": [{"db": d, "position": round(db_to_position(d), 9)} for d in decibels],
    "tolerance": {"db": 1e-6, "position": 1e-7},
}

print(json.dumps(fixture, indent=2))
