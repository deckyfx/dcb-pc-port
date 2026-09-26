#!/usr/bin/env bash
# Import a boot PS-EXE into the shared Ghidra project via ghidra_psx_ldr and auto-analyse it.
#
#   tools/ghidra/import_ghidra.sh SLPS-03101      # after tools/disc/extract_disc.py
#
# Project: ghidra/project/DCB.gpr, one folder per disc serial, program named after the
# on-disc file (SLPS_031.01), so JP and US builds can be compared with Version Tracking.
# Close the project in the Ghidra GUI first: headless and GUI cannot share the lock.
set -euo pipefail

GAME_ID="${1:?usage: $0 <disc serial, e.g. SLPS-03101>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GHIDRA_INSTALL_DIR="${GHIDRA_INSTALL_DIR:-$ROOT/ghidra/ghidra_12.1.2_PUBLIC}"
MANIFEST="$ROOT/extracted/$GAME_ID/manifest.json"
[[ -f "$MANIFEST" ]] || { echo "error: $MANIFEST missing; run tools/disc/extract_disc.py first" >&2; exit 1; }

BOOT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["boot_exe"]["file"])' "$MANIFEST")"
mkdir -p "$ROOT/ghidra/project" "$ROOT/logs"

"$GHIDRA_INSTALL_DIR/support/analyzeHeadless" \
    "$ROOT/ghidra/project" "DCB/$GAME_ID" \
    -import "$ROOT/extracted/$GAME_ID/fs/$BOOT" \
    -loader PsxLoader \
    -overwrite \
    -scriptPath "$ROOT/ghidra/scripts" \
    -log "$ROOT/logs/ghidra_import_$GAME_ID.log"
