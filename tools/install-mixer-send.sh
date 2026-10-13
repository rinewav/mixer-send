#!/usr/bin/env bash
# Install (or uninstall) the built Mixer SEND plugin for the current user.
#
# What it does:
#   1. Copies  native/mixer-send/build/MixerSend_artefacts/Release/VST3/Mixer SEND.vst3
#      to      ~/Library/Audio/Plug-Ins/VST3/Mixer SEND.vst3
#   2. Copies  native/mixer-send/build/MixerSend_artefacts/Release/AU/Mixer SEND.component
#      to      ~/Library/Audio/Plug-Ins/Components/Mixer SEND.component
#
# Only those two bundles are replaced (a previous copy of the SAME bundle is
# removed first, so no stale file survives inside it). Nothing else in either
# folder, no plugin cache and no DAW setting is touched. DAWs pick the plugin
# up on their next scan; one that is already running may need a rescan.
#
# --uninstall removes exactly those two bundles (whichever are present) and
# nothing else. A DAW that has the plugin loaded keeps running it until it
# quits; projects that use it then report it missing.
#
# Migration: before the rename to Touch Console (Covo Design) the plugin was
# com.touchconsole.mixersend (manufacturer "Touch Console", code Tcon). A
# bundle in either folder that still carries that identifier, whatever its
# name, is removed by an install and by --uninstall, so no host lists the
# former plugin beside the current one. (Projects saved with the former
# plugin do not find it under the new identity; they need a new instance.)
#
# Build first:  pnpm mixer-send:build
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ARTEFACTS="${REPO_ROOT}/native/mixer-send/build/MixerSend_artefacts/Release"
VST3_SRC="${ARTEFACTS}/VST3/Mixer SEND.vst3"
AU_SRC="${ARTEFACTS}/AU/Mixer SEND.component"
VST3_DIR="${HOME}/Library/Audio/Plug-Ins/VST3"
AU_DIR="${HOME}/Library/Audio/Plug-Ins/Components"
LEGACY_BUNDLE_ID="com.touchconsole.mixersend"

# The bundles in the two folders that carry the former identifier.
legacy_bundles() {
  local dir bundle id
  for dir in "${VST3_DIR}" "${AU_DIR}"; do
    [[ -d "${dir}" ]] || continue
    for bundle in "${dir}"/*.vst3 "${dir}"/*.component; do
      [[ -f "${bundle}/Contents/Info.plist" ]] || continue
      id="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "${bundle}/Contents/Info.plist" 2>/dev/null || true)"
      if [[ "${id}" == "${LEGACY_BUNDLE_ID}" ]]; then
        printf '%s\n' "${bundle}"
      fi
    done
  done
}

# remove_legacy: prints and (unless --dry-run) removes each former bundle.
remove_legacy() {
  local bundle
  while IFS= read -r bundle; do
    [[ -n "${bundle}" ]] || continue
    if [[ "${DRY_RUN}" -eq 1 ]]; then
      echo "would remove the former: ${bundle}"
    else
      rm -rf "${bundle}"
      echo "removed the former: ${bundle}"
    fi
  done < <(legacy_bundles)
}

DRY_RUN=0
UNINSTALL=0
for arg in "$@"; do
  case "${arg}" in
    --dry-run) DRY_RUN=1 ;;
    --uninstall) UNINSTALL=1 ;;
    --help|-h)
      echo "Usage: install-mixer-send.sh [--uninstall] [--dry-run]"
      echo "  --uninstall remove the two installed bundles"
      echo "  --dry-run   print what would be copied or removed, change nothing"
      exit 0
      ;;
    *) echo "Unknown argument: ${arg}" >&2; exit 2 ;;
  esac
done

if [[ "${UNINSTALL}" -eq 1 ]]; then
  removed=0
  for dest in "${VST3_DIR}/Mixer SEND.vst3" "${AU_DIR}/Mixer SEND.component"; do
    if [[ ! -e "${dest}" && ! -L "${dest}" ]]; then
      echo "not installed: ${dest}"
    elif [[ "${DRY_RUN}" -eq 1 ]]; then
      echo "would remove: ${dest}"
    else
      rm -rf "${dest}"
      echo "removed: ${dest}"
      removed=$((removed + 1))
    fi
  done
  remove_legacy
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "dry-run: nothing removed."
  else
    echo "Mixer SEND uninstalled (${removed} bundle(s) removed)."
  fi
  exit 0
fi

for bundle in "${VST3_SRC}" "${AU_SRC}"; do
  if [[ ! -d "${bundle}" ]]; then
    echo "Not built: ${bundle}" >&2
    echo "Run 'pnpm mixer-send:build' first." >&2
    exit 1
  fi
done

install_bundle() {
  local src="$1" dest_dir="$2"
  local dest="${dest_dir}/$(basename "${src}")"
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "would copy: ${src}"
    echo "        to: ${dest}"
    return
  fi
  mkdir -p "${dest_dir}"
  rm -rf "${dest}"
  # ditto keeps the bundle's code signature and extended attributes intact.
  ditto "${src}" "${dest}"
  echo "installed: ${dest}"
}

# The former bundles first: one of them may have the name the new one takes.
remove_legacy
install_bundle "${VST3_SRC}" "${VST3_DIR}"
install_bundle "${AU_SRC}" "${AU_DIR}"

if [[ "${DRY_RUN}" -eq 0 ]]; then
  version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "${AU_DIR}/Mixer SEND.component/Contents/Info.plist" 2>/dev/null || echo '?')"
  echo "Mixer SEND ${version} installed (VST3 + AU)."
  echo "Check the AU with:  auval -v aufx Msnd Covo"
fi
