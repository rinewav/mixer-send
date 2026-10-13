#!/usr/bin/env bash
# Runs a Windows executable under wine in a dedicated prefix, so build steps
# (JUCE's VST3 moduleinfo helper), unit tests and smoke tests never touch the
# user's own ~/.wine. Used as CMAKE_CROSSCOMPILING_EMULATOR by
# tools/win/clang-cl-x64.cmake.
#
#   TOUCH_CONSOLE_WINEPREFIX  prefix to use (default ~/.cache/touch-console/wine-prefix)
#   TOUCH_CONSOLE_WINE        wine binary (default: wine on PATH, else /opt/homebrew/bin/wine)
set -euo pipefail

export WINEPREFIX="${TOUCH_CONSOLE_WINEPREFIX:-$HOME/.cache/touch-console/wine-prefix}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"   # no Mono / Gecko prompts
export MVK_CONFIG_LOG_LEVEL="${MVK_CONFIG_LOG_LEVEL:-0}"            # wine's MoltenVK banner

wine_bin="${TOUCH_CONSOLE_WINE:-$(command -v wine || echo /opt/homebrew/bin/wine)}"

if [[ ! -f "$WINEPREFIX/system.reg" ]]; then
    mkdir -p "$WINEPREFIX"
    "$wine_bin" wineboot --init >/dev/null 2>&1 || true
    wineserver_bin="$(dirname "$wine_bin")/wineserver"
    if [[ -x "$wineserver_bin" ]]; then "$wineserver_bin" -w || true; fi
fi

# wineboot links Desktop, Documents, Downloads, Music, Pictures and Videos to
# the real folders in $HOME: an installer or plugin under test would write
# there (a smoke-tested NSIS installer put its shortcut on the real Desktop).
# Unlink them (the link only; the targets are untouched) and use empty folders.
for profile_dir in "$WINEPREFIX"/drive_c/users/*/; do
    for folder in Desktop Documents Downloads Music Pictures Videos; do
        if [[ -L "$profile_dir$folder" ]]; then
            rm "$profile_dir$folder"
            mkdir -p "$profile_dir$folder"
        fi
    done
done

exec "$wine_bin" "$@"
