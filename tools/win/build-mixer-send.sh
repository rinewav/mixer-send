#!/usr/bin/env bash
# Cross-compiles Mixer SEND for Windows x64 on macOS (clang-cl + lld-link +
# the xwin CRT/SDK; see tools/win/setup-toolchain.sh):
#
#   native/mixer-send/build-win/MixerSend_artefacts/Release/VST3/Mixer SEND.vst3/
#       Contents/x86_64-win/Mixer SEND.vst3        the plugin DLL (static CRT)
#       Contents/Resources/moduleinfo.json         written by JUCE's helper under wine
#
# plus the unit tests and the mini host as Windows .exe files (run them under
# wine with pnpm mixer-send:verify:win / pnpm mixer-send:e2e:win).
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="$repo/native/mixer-send/build-win"

cmake -S "$repo/native/mixer-send" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$repo/tools/win/clang-cl-x64.cmake"
cmake --build "$build" --target MixerSend_VST3 mixer_send_tests mixer_send_mini_host

echo
echo "Windows VST3: $build/MixerSend_artefacts/Release/VST3/Mixer SEND.vst3"
