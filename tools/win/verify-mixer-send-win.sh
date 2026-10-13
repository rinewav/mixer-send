#!/usr/bin/env bash
# Checks the cross-compiled Windows build of Mixer SEND on this Mac, as far as
# a Mac can: PE headers and architecture, exports, imported DLLs (no Visual
# C++ redistributable), the VST3 bundle layout and moduleinfo.json, the unit
# tests (Windows .exe) under wine, and pluginval for Windows under wine.
#
#   bash tools/win/verify-mixer-send-win.sh            # everything
#   bash tools/win/verify-mixer-send-win.sh --no-pluginval
#
# Env: TOUCH_CONSOLE_LLVM (default /opt/homebrew/opt/llvm@22),
#      PLUGINVAL_STRICTNESS (default 10), PLUGINVAL_GUI=1 (also open the
#      editor; wine's Direct2D may not cope), PLUGINVAL_EXE (default: downloaded
#      pluginval 1.0.4 for Windows under ~/.cache/touch-console/pluginval-win).
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="$repo/native/mixer-send/build-win"
bundle="$build/MixerSend_artefacts/Release/VST3/Mixer SEND.vst3"
dll="$bundle/Contents/x86_64-win/Mixer SEND.vst3"
llvm="${TOUCH_CONSOLE_LLVM:-/opt/homebrew/opt/llvm@22}"
cache="$HOME/.cache/touch-console"
wine_run="$repo/tools/win/wine-run.sh"
run_pluginval=1
[[ "${1:-}" == "--" ]] && shift  # pnpm 11 forwards a bare --
[[ "${1:-}" == "--no-pluginval" ]] && run_pluginval=0

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok    $*"; }

[[ -f "$dll" ]] || fail "no build at $dll (run pnpm mixer-send:build:win)"

# 1. Bundle layout (VST3 SDK "Plugin Format", Windows bundle)
[[ -f "$bundle/Contents/Resources/moduleinfo.json" ]] || fail "moduleinfo.json missing"
pass "bundle: Contents/x86_64-win/Mixer SEND.vst3 + Contents/Resources/moduleinfo.json"

# 2. PE headers
headers="$("$llvm/bin/llvm-readobj" --file-headers "$dll")"
grep -q "IMAGE_FILE_MACHINE_AMD64" <<<"$headers" || fail "not an x64 image"
grep -q "IMAGE_FILE_DLL" <<<"$headers" || fail "not a DLL"
pass "PE32+ DLL, machine AMD64"

# 3. Exports
exports="$("$llvm/bin/llvm-objdump" -p "$dll" | sed -n '/Export Table/,/^$/p')"
for symbol in GetPluginFactory InitDll ExitDll; do
    grep -q " $symbol\$" <<<"$exports" || fail "export $symbol missing"
done
pass "exports GetPluginFactory, InitDll, ExitDll"

# 4. Imports: only DLLs that ship with Windows (static CRT, no VC++ redist)
imports="$("$llvm/bin/llvm-objdump" -p "$dll" | awk '/DLL Name:/ {print $3}' | sort -u | tr '\n' ' ')"
if grep -qiE "vcruntime|msvcp|ucrtbase|api-ms-win-crt" <<<"$imports"; then
    fail "depends on the Visual C++ runtime: $imports"
fi
pass "imports: $imports"

# 5. moduleinfo.json names the class the DLL exports (JSON5: trailing commas)
moduleinfo="$bundle/Contents/Resources/moduleinfo.json"
grep -q '"Name": "Mixer SEND"' "$moduleinfo" || fail "moduleinfo: name"
grep -q '"Category": "Audio Module Class"' "$moduleinfo" || fail "moduleinfo: audio module class"
grep -q '"Vendor": "Covo Design"' "$moduleinfo" || fail "moduleinfo: vendor"
pass "moduleinfo.json: $(grep -m1 '"CID"' "$moduleinfo" | tr -d ' ,')"

# 6. Unit tests (the Windows build of core + bus + Winsock endpoint) under wine
if [[ -f "$build/mixer_send_tests_artefacts/Release/mixer_send_tests.exe" ]]; then
    MVK_CONFIG_LOG_LEVEL=0 "$wine_run" "$build/mixer_send_tests_artefacts/Release/mixer_send_tests.exe" \
        2>/dev/null | grep -E "tests passed|FAIL" | tail -3
else
    echo "skip  unit tests (build the mixer_send_tests target)"
fi

# 7. pluginval (Windows build) under wine
if [[ $run_pluginval == 1 ]]; then
    exe="${PLUGINVAL_EXE:-$cache/pluginval-win/pluginval.exe}"
    if [[ ! -f "$exe" ]]; then
        mkdir -p "$(dirname "$exe")"
        curl -fsSL ${HTTPS_PROXY:+-x "$HTTPS_PROXY"} -o "$(dirname "$exe")/pluginval_Windows.zip" \
            https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Windows.zip
        (cd "$(dirname "$exe")" && unzip -o -q pluginval_Windows.zip)
    fi
    windows_path="Z:$(tr '/' '\\' <<<"$bundle")"
    log="$build/pluginval-wine${PLUGINVAL_GUI:+-gui}.log"
    # A dead port: never talk to a real companion from a validation run.
    gui_flag="--skip-gui-tests"
    [[ "${PLUGINVAL_GUI:-0}" == 1 ]] && gui_flag=""
    set +e
    MIXER_SEND_COMPANION_PORT="${MIXER_SEND_COMPANION_PORT:-48999}" MVK_CONFIG_LOG_LEVEL=0 \
        "$wine_run" "$exe" --strictness-level "${PLUGINVAL_STRICTNESS:-10}" $gui_flag \
        --timeout-ms 300000 --validate "$windows_path" >"$log" 2>&1
    status=$?
    set -e
    tail -5 "$log"
    [[ $status == 0 ]] || fail "pluginval exit $status (log: $log)"
    pass "pluginval ${PLUGINVAL_STRICTNESS:-10} under wine (log: $log)"
fi
