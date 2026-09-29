#!/usr/bin/env bash
# Package a release: build Linux + Windows binaries, compile the asset pack from the
# player's own dumps, and zip per-OS bundles ready to ship.
#
#   ./pack.sh [-s SERIAL] [-o OUTDIR]
#
# Needs the player's own imported dumps (extracted/<serial>/, JP and optionally US for the
# English data); nothing copyrighted ships in the repo. The zips bundle the player's own
# extracted/<serial>/ (the game data the build runs from) plus the compiled <serial>.pak,
# so the game boots with no import step. Produces, in OUTDIR (default dist/):
#   dcb-pc-v<version>-linux.zip     dcb (linux-release) + extracted/ + <serial>.pak + README
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
echo "-- assets: rip + English data + movies + pack"
./build/linux-release/dcb_asset_ripper unpack "extracted/$SERIAL" >/dev/null
./build/linux-release/dcb_asset_ripper sfx "assets/raw/$SERIAL" --game "$SERIAL" >/dev/null
# The rip rewrites every PNG in converted/: put hand-edited images back over it.
# assets/<serial>/custom/textures/ mirrors converted/<serial>/textures/ (e.g. the
# English title art); keep your edits there, not only in converted/.
if [[ -d "$ASSETS/custom/textures" ]]; then
    cp -r "$ASSETS/custom/textures/." "assets/converted/$SERIAL/textures/"
    echo "   custom images: $(find "$ASSETS/custom/textures" -type f | wc -l) restored over the rip"
fi
# sprites.txt (sprite scale rules, e.g. the wider title art) lives in converted/ too.
if [[ -f "$ASSETS/custom/sprites.txt" ]]; then
    cp "$ASSETS/custom/sprites.txt" "assets/converted/$SERIAL/sprites.txt"
    echo "   sprites.txt restored"
fi
if [[ -d "extracted/SLUS-01328" ]]; then
    ./build/linux-release/dcb_asset_ripper unpack "extracted/SLUS-01328" >/dev/null
    python3 tools/text/en_text.py --jp "extracted/SLPS-03101" --us extracted/SLUS-01328 \
        --out "$ASSETS" >/dev/null
    # The rip above rewrote the texture manifest (JP art only): put the US art back
    # before packing. Its summary is shown so a failed swap is not missed.
    python3 tools/assets/swap_us_images.py --apply 2>&1 | grep -E "^replaced|error" || true
fi
# Movies go into converted/ BEFORE the pack: the running game only reads
# movie/movie<N>.mpg from the .pak (or a loose asset folder), never movie_src/.
python3 tools/disc/rip_movies.py --serial "$SERIAL" >/dev/null
./build/linux-release/dcb_asset_ripper pack "assets/converted/$SERIAL" "assets/$SERIAL.pak"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$OUTDIR" "$STAGE/linux" "$STAGE/windows"
cat > "$STAGE/README.txt" <<EOF
Digimon Digital Card Battle PC v$VERSION ($SERIAL)
=================================================

Run dcb (Linux) or dcb.exe (Windows) from this folder. No BIOS, disc, or import
step needed: extracted/$SERIAL/ (your own dump, bundled by pack.sh) is the game
data, and assets/$SERIAL/ holds the English/compiled data.

Cheats live in cheats/<serial>.txt (F4 in game, a starter file is included);
saves in saves/<serial>/. Settings (window size, keys, gamepad, volume) are
written to settings.ini on first run.
EOF
for OS in linux windows; do
    # assets/<serial>.pak: the ONLY texture/movie/sfx source the game mounts
    # (packed from converted/, movies included). It must sit at assets/<serial>.pak:
    # assets/<serial>/<serial>.pak is an unused leftover some runs produce.
    # assets/<serial>/: the loose English files the game reads directly
    # (en_font.bin, en_bigfont.bin, en_names.txt, text/ catalog, files/ for
    # CARD2.CDD etc.).
    mkdir -p "$STAGE/$OS/assets/$SERIAL"
    cp "assets/$SERIAL.pak" "$STAGE/$OS/assets/"
    for f in en_font.bin en_bigfont.bin en_names.txt; do
        if [[ -f "$ASSETS/$f" ]]; then cp "$ASSETS/$f" "$STAGE/$OS/assets/$SERIAL/"; fi
    done
    if [[ -d "$ASSETS/files" ]]; then cp -r "$ASSETS/files" "$STAGE/$OS/assets/$SERIAL/"; fi
    # text/: the translation catalog (menus, dialogs); without it they stay Japanese.
    if [[ -d "$ASSETS/text" ]]; then cp -r "$ASSETS/text" "$STAGE/$OS/assets/$SERIAL/"; fi
    cp "$STAGE/README.txt" "$STAGE/$OS/"
    # cheats/<serial>.txt bootstrap (the trainer saves back to it).
    mkdir -p "$STAGE/$OS/cheats"
    if [[ -f "cheats/$SERIAL.txt" ]]; then
        cp "cheats/$SERIAL.txt" "$STAGE/$OS/cheats/$SERIAL.txt"
    else
        cp docs/cheats.example.txt "$STAGE/$OS/cheats/$SERIAL.txt"
    fi
    mkdir -p "$STAGE/$OS/extracted"
    cp -r "extracted/$SERIAL" "$STAGE/$OS/extracted/"
done
cp build/linux-release/dcb "$STAGE/linux/"
cp build/windows-cross/dcb.exe "$STAGE/windows/"

LINUX_ZIP="$OUTDIR/dcb-pc-v$VERSION-linux.zip"
WIN_ZIP="$OUTDIR/dcb-pc-v$VERSION-windows.zip"
(cd "$STAGE/linux" && zip -q -9 -r "$OLDPWD/$LINUX_ZIP" dcb README.txt assets extracted cheats)
(cd "$STAGE/windows" && zip -q -9 -r "$OLDPWD/$WIN_ZIP" dcb.exe README.txt assets extracted cheats)
ls -la "$LINUX_ZIP" "$WIN_ZIP"
echo "== packed v$VERSION =="
