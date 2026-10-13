# Mixer SEND

Mixer SEND is the VST3 / AU plug-in of [Touch Console](https://touchconsole.covodesign.net) by Covo
Design. Insert it on a DAW track and Touch Console shows the track on a tablet or phone, where its
volume, pan and mute can be controlled. The plug-in talks only to Touch Console on the same
computer (UDP on 127.0.0.1).

This repository is the complete source code of the Mixer SEND that Touch Console ships, under the
GNU Affero General Public License, version 3 ([LICENSE](LICENSE)). Each tag names the Touch Console
release that contains it:

| Tag | Mixer SEND | Shipped in |
| --- | --- | --- |
| `touch-console-1.0.2` | 1.0.1 | Touch Console 1.0.2 (macOS universal VST3 + AU, Windows x64 VST3) |

Mixer SEND, Copyright (C) 2026 Covo Design. It comes with ABSOLUTELY NO WARRANTY. Touch Console
itself is a separate, closed-source program and is not part of this repository.

## What the binaries include

Mixer SEND is built with [JUCE](https://github.com/juce-framework/JUCE) 8.0.12, unmodified, used under
the AGPLv3. A copy of that JUCE source is attached to each release here. From JUCE, the binaries
contain the following code, whose licences are in the JUCE source at the paths given:

| Component | Licence | In JUCE 8.0.12 |
| --- | --- | --- |
| Steinberg VST 3 SDK 3.8.0 | MIT | `modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt` |
| Apple AudioUnitSDK (AU only) | Apache-2.0 | `modules/juce_audio_plugin_client/AU/AudioUnitSDK/LICENSE.txt` |
| zlib 1.3.1 | zlib | `modules/juce_core/zip/zlib/LICENSE` |
| libpng 1.6.37 | PNG Reference Library License v2 | `modules/juce_graphics/image_formats/pnglib/LICENSE` |
| IJG libjpeg 9f | IJG | `modules/juce_graphics/image_formats/jpglib/README` |
| HarfBuzz 10.1.0 | Old MIT | `modules/juce_graphics/fonts/harfbuzz/COPYING` |
| SheenBidi 2.9.0 | Apache-2.0 | `modules/juce_graphics/unicode/sheenbidi/LICENSE` |

VST is a registered trademark of Steinberg Media Technologies GmbH. This software is based in part
on the work of the Independent JPEG Group.

## Building

You need JUCE 8.0.12 (`-DJUCE_PATH=/path/to/JUCE`, default `~/JUCE`), CMake 3.22 or later and Ninja.

macOS, universal VST3 and AU (Xcode or the Command Line Tools):

```bash
cmake -S native/mixer-send -B native/mixer-send/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build native/mixer-send/build
ctest --test-dir native/mixer-send/build --output-on-failure
bash tools/install-mixer-send.sh            # into ~/Library/Audio/Plug-Ins (--uninstall removes it)
```

Windows x64 VST3, cross-compiled on macOS with clang-cl, lld-link and the Microsoft CRT and Windows
SDK fetched by xwin (under Microsoft's licence, accepted by you when the script runs); wine runs
JUCE's helper that writes `Contents/Resources/moduleinfo.json`:

```bash
brew install --cask wine-stable             # without wine, moduleinfo.json and the tests are skipped
bash tools/win/setup-toolchain.sh           # Homebrew llvm@22, lld@22, xwin, cmake, ninja
bash tools/win/build-mixer-send.sh
bash tools/win/verify-mixer-send-win.sh     # exports, imports, unit tests and pluginval under wine
```

The shipped bundles are the build output plus two steps that Touch Console's packaging does: on macOS
each bundle is signed ad hoc (`codesign --force --deep --sign - --timestamp=none`), and on both
platforms each bundle gets this repository's `LICENSE` as `Contents/Resources/LICENSE.txt`.

The unit tests read golden vectors in `packages/protocol/fixtures`, which Touch Console's own
TypeScript tests share; the Python scripts beside them regenerate them.

`native/mixer-send/tools/host_harness.cpp` has an end-to-end mode that pairs with a running Touch
Console server; the other tools (`mini_host`, `ui_snapshot`) need nothing else.
