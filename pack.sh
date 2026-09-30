#!/usr/bin/env bash
# Package a release: build Linux + Windows binaries, compile the asset pack from the
# player's own dumps, and zip per-OS bundles ready to ship.
#
#   ./pack.sh [-s SERIAL] [-o OUTDIR]
#
# Needs the player's own imported dumps (extracted/<serial>/, JP and optionally US for the
# English data); nothing copyrighted ships in the repo. Produces, in OUTDIR (default dist/):
#   dcb-pc-v<version>-linux.zip     dcb (linux-release) + <serial>.pak + README + settings.ini
#   dcb-pc-v<version>-windows.zip   dcb.exe (windows-cross) + the same payload
#
# The version is the CMake project VERSION (x.y.z); the window title carries it too.
set -euo pipefail
cd "$(dirname "$0")"

SERIAL="SLPS-03101"
OUTDIR="dist"
while getopts "s:o:h" opt; do
    case "$opt" in
        s) SERIAL="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        *) sed -n '2,12p' "$0"; exit 2 ;;
    esac
done

VERSION="$(grep -m1 -A2 '^project(' CMakeLists.txt | grep -o '[0-9][0-9.]*' | head -1)"
[[ -n "$VERSION" ]] || { echo "error: cannot read project VERSION from CMakeLists.txt" >&2; exit 1; }
[[ -d "extracted/$SERIAL" ]] || { echo "error: import the $SERIAL dump first (dcb --import)" >&2; exit 1; }
command -v zip >/dev/null || { echo "error: zip is not installed" >&2; exit 1; }

echo "== pack v$VERSION ($SERIAL) =="

echo "-- build: linux-release"
cmake --preset linux-release -DDCB_GAME_ID="$SERIAL" >/dev/null
cmake --build --preset linux-release --target dcb dcb_asset_ripper 2>&1 | tail -1

echo "-- build: windows-cross"
cmake --preset windows-cross -DDCB_GAME_ID="$SERIAL" >/dev/null
cmake --build --preset windows-cross --target dcb 2>&1 | tail -1

ASSETS="assets/$SERIAL"
if [[ ! -f "$ASSETS/$SERIAL.pak" ]]; then
    echo "-- assets: rip + English data + pack"
    ./build/linux-release/dcb_asset_ripper unpack "extracted/$SERIAL" >/dev/null
    ./build/linux-release/dcb_asset_ripper sfx "assets/raw/$SERIAL" --game "$SERIAL" >/dev/null
    if [[ -d "extracted/SLUS-01328" ]]; then
        ./build/linux-release/dcb_asset_ripper unpack "extracted/SLUS-01328" >/dev/null
        python3 tools/text/en_text.py --jp "extracted/SLPS-03101" --us extracted/SLUS-01328 \
            --out "$ASSETS" >/dev/null
        python3 tools/assets/swap_us_images.py --apply >/dev/null
    fi
    ./build/linux-release/dcb_asset_ripper pack "assets/converted/$SERIAL" "$ASSETS/$SERIAL.pak"
else
    echo "-- assets: $ASSETS/$SERIAL.pak is up to date"
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$OUTDIR" "$STAGE/linux" "$STAGE/windows"
cat > "$STAGE/README.txt" <<EOF
Digimon Digital Card Battle PC v$VERSION ($SERIAL)
=================================================

Run dcb (Linux) or dcb.exe (Windows). No BIOS or disc needed: import your own
Japanese SLPS-03101 dump once (file picker on first run, or: dcb --import <disc.cue>),
and optionally your US SLUS-01328 dump for English text and art.

$SERIAL.pak holds textures/sound compiled from the player's own dumps.
Cheats live in cheats/<serial>.txt (F4 in game); saves in saves/<serial>/.
Settings (window size, keys, gamepad, volume) are written to settings.ini on first run.
EOF
cp "$ASSETS/$SERIAL.pak" "$STAGE/linux/"
cp "$ASSETS/$SERIAL.pak" "$STAGE/windows/"
cp build/windows-cross/dcb.exe "$STAGE/windows/"

LINUX_ZIP="$OUTDIR/dcb-pc-v$VERSION-linux.zip"
WIN_ZIP="$OUTDIR/dcb-pc-v$VERSION-windows.zip"
(cd "$STAGE/linux" && zip -q -9 "$OLDPWD/$LINUX_ZIP" dcb "$SERIAL.pak" README.txt)
(cd "$STAGE/windows" && zip -q -9 "$OLDPWD/$WIN_ZIP" dcb.exe "$SERIAL.pak" README.txt)
ls -la "$LINUX_ZIP" "$WIN_ZIP"
echo "== packed v$VERSION =="
