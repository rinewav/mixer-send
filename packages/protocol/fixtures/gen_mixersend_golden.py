#!/usr/bin/env python3
"""Independent (third) encoder for the Mixer SEND UDP wire, protocol v1.

It exists only to produce `mixersend-golden.json`. Neither the TypeScript
codec (packages/protocol/src/mixersend.ts) nor the C++ codec
(native/mixer-send/src/core/WireProtocol.cpp) is involved in generating the
bytes, so both are checked against something that is not themselves.

The committed JSON is the contract. Regenerate it only when the wire changes
(and bump the protocol version when it does):

    python3 packages/protocol/fixtures/gen_mixersend_golden.py \
        > packages/protocol/fixtures/mixersend-golden.json
    pnpm exec biome format --write packages/protocol/fixtures/mixersend-golden.json
"""

import json
import struct

MAGIC = b"MXSN"
VERSION = 1
TYPES = dict(
    hello=0x01, info=0x02, state=0x03, bye=0x04,
    welcome=0x10, control=0x11, lease=0x12, reassign=0x13,
)


def header(kind, count, sender):
    return MAGIC + struct.pack("<BBHQ", VERSION, TYPES[kind], count, sender)


def hexid(value):
    return "%016x" % value


BUS = 0x0123456789ABCDEF
COMPANION = 0xFEDCBA9876543210
A = 0x1111222233334444
B = 0x5555666677778888
C = 0x9A0B1C2D3E4F5061

vectors = []


def add(name, direction, data, message=None, error=None, note=None):
    vector = {"name": name, "direction": direction, "hex": data.hex()}
    if note:
        vector["note"] = note
    if message is not None:
        vector["message"] = message
    if error is not None:
        vector["error"] = error
    vectors.append(vector)


def hello(pid, version, fmt, tick, count, host, sender=BUS, header_count=0):
    encoded = host.encode()
    return header("hello", header_count, sender) + struct.pack(
        "<IIBBHB", pid, version, fmt, tick, count, len(encoded)) + encoded


def info_record(instance, channels, colour, name):
    encoded = name.encode() if name is not None else b""
    flags = 1 if colour is not None else 0
    return struct.pack("<QBBIB", instance, channels, flags, colour or 0, len(encoded)) + encoded


def state_record(instance, volume, pan, flags, pl, pr, rl, rr, ack):
    return struct.pack("<QHbBBBBBH", instance, volume, pan, flags, pl, pr, rl, rr, ack)


def control_record(instance, seq, mask, volume, pan, mute):
    return struct.pack("<QHBHbB", instance, seq, mask, volume, pan, mute)


def lease_record(instance, flags, duration):
    return struct.pack("<QBH", instance, flags, duration)


def target(bus=BUS):
    return struct.pack("<Q", bus)


# --- plugin -> companion ----------------------------------------------------

add("hello-vst3", "plugin-to-companion",
    hello(4242, (0 << 16) | (1 << 8) | 0, 1, 30, 3, "Steinberg Cubase"),
    {"type": "hello", "senderId": hexid(BUS), "pid": 4242, "pluginVersion": "0.1.0",
     "format": "vst3", "tickHz": 30, "instanceCount": 3, "hostName": "Steinberg Cubase"})

add("hello-au-no-host-name", "plugin-to-companion",
    hello(1, (2 << 16) | (10 << 8) | 255, 2, 1, 0, ""),
    {"type": "hello", "senderId": hexid(BUS), "pid": 1, "pluginVersion": "2.10.255",
     "format": "au", "tickHz": 1, "instanceCount": 0, "hostName": ""})

add("info-two-records", "plugin-to-companion",
    header("info", 2, BUS) + info_record(A, 2, 0xFF3A7BD5, "Kick") + info_record(B, 1, None, None),
    {"type": "info", "senderId": hexid(BUS), "instances": [
        {"instanceId": hexid(A), "channels": 2, "colour": "#ff3a7bd5", "name": "Kick"},
        {"instanceId": hexid(B), "channels": 1}]},
    note="An absent colour and an absent name are MISSING KEYS, never defaults.")

add("info-utf8-name", "plugin-to-companion",
    header("info", 1, BUS) + info_record(C, 2, 0x80000000, "ボーカル"),
    {"type": "info", "senderId": hexid(BUS), "instances": [
        {"instanceId": hexid(C), "channels": 2, "colour": "#80000000", "name": "ボーカル"}]})

add("state-two-records", "plugin-to-companion",
    header("state", 2, BUS)
    + state_record(A, 12451, 0, 0b0100, 180, 175, 150, 148, 0)
    + state_record(B, 0, -100, 0b1011, 0, 0, 0, 0, 65535),
    {"type": "state", "senderId": hexid(BUS), "instances": [
        {"instanceId": hexid(A), "volume14": 12451, "pan": 0, "muted": False, "leaseMuted": False,
         "processing": True, "bypassed": False,
         "peakL": 180, "peakR": 175, "rmsL": 150, "rmsR": 148, "ackSeq": 0},
        {"instanceId": hexid(B), "volume14": 0, "pan": -100, "muted": True, "leaseMuted": True,
         "processing": False, "bypassed": True,
         "peakL": 0, "peakR": 0, "rmsL": 0, "rmsR": 0, "ackSeq": 65535}]})

add("state-extremes", "plugin-to-companion",
    header("state", 1, BUS) + state_record(C, 16383, 100, 0, 255, 255, 255, 255, 1),
    {"type": "state", "senderId": hexid(BUS), "instances": [
        {"instanceId": hexid(C), "volume14": 16383, "pan": 100, "muted": False, "leaseMuted": False,
         "processing": False, "bypassed": False,
         "peakL": 255, "peakR": 255, "rmsL": 255, "rmsR": 255, "ackSeq": 1}]})

add("bye-instances", "plugin-to-companion",
    header("bye", 2, BUS) + struct.pack("<QQ", A, B),
    {"type": "bye", "senderId": hexid(BUS), "instanceIds": [hexid(A), hexid(B)]})

add("bye-bus", "plugin-to-companion", header("bye", 0, BUS),
    {"type": "bye", "senderId": hexid(BUS), "instanceIds": []},
    note="count 0: the whole bus (every instance of that binary in that process) is going away.")

# --- companion -> plugin ----------------------------------------------------

add("welcome", "companion-to-plugin", header("welcome", 0, COMPANION) + target(),
    {"type": "welcome", "senderId": hexid(COMPANION), "targetBusId": hexid(BUS)})

add("control-two-records", "companion-to-plugin",
    header("control", 2, COMPANION) + target()
    + control_record(A, 1, 0b001, 12451, 0, 0)
    + control_record(B, 65535, 0b110, 0, 100, 1),
    {"type": "control", "senderId": hexid(COMPANION), "targetBusId": hexid(BUS), "controls": [
        {"instanceId": hexid(A), "seq": 1, "volume14": 12451},
        {"instanceId": hexid(B), "seq": 65535, "pan": 100, "muted": True}]},
    note="Fields outside the mask travel as zero and decode as missing keys.")

add("control-all-fields", "companion-to-plugin",
    header("control", 1, COMPANION) + target() + control_record(C, 7, 0b111, 0, -100, 0),
    {"type": "control", "senderId": hexid(COMPANION), "targetBusId": hexid(BUS), "controls": [
        {"instanceId": hexid(C), "seq": 7, "volume14": 0, "pan": -100, "muted": False}]})

add("lease-mute-and-release", "companion-to-plugin",
    header("lease", 2, COMPANION) + target() + lease_record(A, 1, 1000) + lease_record(B, 0, 0),
    {"type": "lease", "senderId": hexid(COMPANION), "targetBusId": hexid(BUS), "leases": [
        {"instanceId": hexid(A), "mute": True, "durationMs": 1000},
        {"instanceId": hexid(B), "mute": False, "durationMs": 0}]})

add("reassign", "companion-to-plugin",
    header("reassign", 1, COMPANION) + target() + struct.pack("<Q", A),
    {"type": "reassign", "senderId": hexid(COMPANION), "targetBusId": hexid(BUS),
     "instanceIds": [hexid(A)]})

# --- malformed: every decoder must reject these -----------------------------

good = header("state", 1, BUS) + state_record(A, 100, 0, 0, 0, 0, 0, 0, 0)
P2C = "plugin-to-companion"
C2P = "companion-to-plugin"

add("reject-bad-magic", P2C, b"MXSX" + good[4:], error="magic")
add("reject-version-2", P2C, good[:4] + bytes([2]) + good[5:], error="version")
add("reject-unknown-type", P2C, good[:5] + bytes([0x7F]) + good[6:], error="type")
add("reject-short-header", P2C, good[:10], error="length")
add("reject-empty", P2C, b"", error="length")
add("reject-trailing-byte", P2C, good + b"\x00", error="length")
add("reject-count-mismatch", P2C,
    header("state", 2, BUS) + state_record(A, 100, 0, 0, 0, 0, 0, 0, 0), error="length")
add("reject-state-count-zero", P2C, header("state", 0, BUS), error="length")
add("reject-volume-out-of-range", P2C,
    header("state", 1, BUS) + state_record(A, 16384, 0, 0, 0, 0, 0, 0, 0), error="range")
add("reject-pan-out-of-range", P2C,
    header("state", 1, BUS) + state_record(A, 0, 101, 0, 0, 0, 0, 0, 0), error="range")
add("reject-reserved-state-flag", P2C,
    header("state", 1, BUS) + state_record(A, 0, 0, 0x10, 0, 0, 0, 0, 0), error="range")
add("reject-zero-instance-id", P2C,
    header("state", 1, BUS) + state_record(0, 0, 0, 0, 0, 0, 0, 0, 0), error="range")
add("reject-zero-sender-id", P2C,
    header("state", 1, 0) + state_record(A, 0, 0, 0, 0, 0, 0, 0, 0), error="range")
add("reject-hello-with-count", P2C, hello(1, 1, 1, 30, 0, "", header_count=1), error="length")
add("reject-hello-unknown-format", P2C, hello(1, 1, 9, 30, 0, ""), error="range")
add("reject-hello-tick-zero", P2C, hello(1, 1, 1, 0, 0, ""), error="range")
add("reject-info-channels-3", P2C, header("info", 1, BUS) + info_record(A, 3, None, "x"), error="range")
add("reject-info-name-too-long", P2C,
    header("info", 1, BUS) + struct.pack("<QBBIB", A, 2, 0, 0, 64) + b"a" * 64, error="range")
add("reject-info-name-bad-utf8", P2C,
    header("info", 1, BUS) + struct.pack("<QBBIB", A, 2, 0, 0, 2) + b"\xc3\x28", error="range")
add("reject-info-name-control-char", P2C,
    header("info", 1, BUS) + struct.pack("<QBBIB", A, 2, 0, 0, 2) + b"a\n", error="range")
add("reject-info-colour-without-flag", P2C,
    header("info", 1, BUS) + struct.pack("<QBBIB", A, 2, 0, 0xFF000000, 0), error="range")
add("reject-info-name-truncated", P2C,
    header("info", 1, BUS) + struct.pack("<QBBIB", A, 2, 0, 0, 5) + b"abc", error="length")
add("reject-control-mask-zero", C2P,
    header("control", 1, COMPANION) + target() + control_record(A, 1, 0, 0, 0, 0), error="range")
add("reject-control-unknown-mask-bit", C2P,
    header("control", 1, COMPANION) + target() + control_record(A, 1, 0b1001, 5, 0, 0), error="range")
add("reject-control-unmasked-field-set", C2P,
    header("control", 1, COMPANION) + target() + control_record(A, 1, 0b001, 5, 3, 0), error="range")
add("reject-control-mute-2", C2P,
    header("control", 1, COMPANION) + target() + control_record(A, 1, 0b100, 0, 0, 2), error="range")
add("reject-control-seq-zero", C2P,
    header("control", 1, COMPANION) + target() + control_record(A, 0, 0b001, 5, 0, 0), error="range")
add("reject-lease-too-long", C2P,
    header("lease", 1, COMPANION) + target() + lease_record(A, 1, 60001), error="range")
add("reject-lease-reserved-flag", C2P,
    header("lease", 1, COMPANION) + target() + lease_record(A, 3, 100), error="range")
add("reject-welcome-zero-target", C2P, header("welcome", 0, COMPANION) + target(0), error="range")
add("reject-welcome-with-count", C2P, header("welcome", 1, COMPANION) + target(), error="length")

fixture = {
    "protocol": "mixer-send",
    "version": VERSION,
    "description": (
        "Golden byte vectors for the Mixer SEND UDP wire (docs/MIXER_SEND.md), generated by "
        "gen_mixersend_golden.py, an encoder independent of both codecs. The TypeScript codec "
        "(packages/protocol/src/mixersend.ts) and the C++ codec "
        "(native/mixer-send/src/core/WireProtocol.cpp) must each decode every 'message' vector "
        "to exactly that object, re-encode the object to exactly those bytes, and reject every "
        "'error' vector. 'error' names the class of the fault: magic, version, type, length, range."
    ),
    "vectors": vectors,
}

print(json.dumps(fixture, indent=2, ensure_ascii=False))
