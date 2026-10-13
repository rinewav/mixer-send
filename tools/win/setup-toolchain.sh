#!/usr/bin/env bash
# One-time setup of the Windows x64 cross toolchain on macOS (Apple Silicon or
# Intel): LLVM clang-cl + lld-link, the MSVC CRT + Windows SDK fetched by
# `xwin`, and a dedicated wine prefix for running built .exe files.
#
#   bash tools/win/setup-toolchain.sh --accept-license
#
# --accept-license is passed on to xwin: the MSVC CRT and the Windows SDK are
# Microsoft software under the Visual Studio / Build Tools licence terms
# (https://go.microsoft.com/fwlink/?LinkId=2086102). Without the flag xwin
# shows the licence and asks. Nothing downloaded here is committed or
# redistributed: it stays in the cache below and is only used to build.
#
# Pinned for reproducible builds (override with the variables below):
#   VS manifest 17 (release channel), Windows SDK 10.0.26100, MSVC CRT 14.44.17.14
#
# Env:
#   TOUCH_CONSOLE_WINSYSROOT  output (default ~/.cache/touch-console/winsysroot)
#   XWIN_SDK_VERSION / XWIN_CRT_VERSION / XWIN_MANIFEST_VERSION
#   HTTPS_PROXY               passed to xwin when the network requires a proxy.
#                             A per-app firewall (LuLu, Little Snitch) must be
#                             answered for xwin itself; do not route around it.
set -euo pipefail

accept=()
for arg in "$@"; do
    case "$arg" in
        --accept-license) accept=(--accept-license) ;;
        --) ;;  # pnpm 11 forwards a bare --
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

cache="$HOME/.cache/touch-console"
sysroot="${TOUCH_CONSOLE_WINSYSROOT:-$cache/winsysroot}"
sdk="${XWIN_SDK_VERSION:-10.0.26100}"
crt="${XWIN_CRT_VERSION:-14.44.17.14}"
manifest="${XWIN_MANIFEST_VERSION:-17}"

need=()
[[ -x /opt/homebrew/opt/llvm@22/bin/clang-cl || -n "${TOUCH_CONSOLE_LLVM:-}" ]] || need+=(llvm@22)
[[ -x /opt/homebrew/opt/lld@22/bin/lld-link || -n "${TOUCH_CONSOLE_LLD:-}" ]] || need+=(lld@22)
command -v xwin >/dev/null || need+=(xwin)
command -v cmake >/dev/null || need+=(cmake)
command -v ninja >/dev/null || need+=(ninja)
if ((${#need[@]})); then
    echo "Installing with Homebrew: ${need[*]}"
    HOMEBREW_NO_AUTO_UPDATE=1 brew install "${need[@]}"
fi
if ! command -v wine >/dev/null; then
    echo "note: wine is not installed (brew install --cask wine-stable). Builds still work;"
    echo "      moduleinfo.json, the unit tests and the smoke tests under wine are skipped."
fi

if [[ -d "$sysroot/VC/Tools/MSVC/$crt" && -d "$sysroot/Windows Kits/10/Include/$sdk" ]]; then
    echo "MSVC CRT $crt + Windows SDK $sdk already at $sysroot"
else
    proxy=()
    [[ -n "${HTTPS_PROXY:-}" ]] && proxy=(--https-proxy "$HTTPS_PROXY")
    mkdir -p "$cache"
    xwin ${accept[@]+"${accept[@]}"} ${proxy[@]+"${proxy[@]}"} --cache-dir "$cache/xwin-cache" \
        --manifest-version "$manifest" --sdk-version "$sdk" --crt-version "$crt" --arch x86_64 \
        splat --use-winsysroot-style --preserve-ms-arch-notation --output "$sysroot"
fi

if command -v wine >/dev/null; then
    # Creates the dedicated prefix (~/.cache/touch-console/wine-prefix) once.
    "$(dirname "$0")/wine-run.sh" cmd /c exit 0 >/dev/null 2>&1 || true
fi

echo "Windows toolchain ready: winsysroot $sysroot"
echo "Next: pnpm mixer-send:build:win && pnpm mixer-send:verify:win"
