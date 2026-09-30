#!/usr/bin/env bash
# Package a release: build Linux + Windows binaries and zip per-OS, self-contained bundles
# (binary + everything under assets/) from the player's own dumps.
#
#   ./pack.sh [-s SERIAL] [-o OUTDIR] [-r] [-1]
#
#   (default)  ship the assets you run with: assets/<serial>.pak and the loose files in
#              assets/<serial>/, exactly as ./dcb.sh uses them. Nothing is regenerated.
#   -r         rebuild them first from the dumps (rip, custom art, English data, US art swap,
#              movies, pack) in build/pack-work/ and ship those. Your assets/ is never
#              written: a rebuild that goes wrong cannot break the working copy.
#   -1         single files instead of zips: the binary with the bundle appended (see below).
#
# Needs the player's own dumps (the JP one at assets/dump/<serial>/ or extracted/<serial>/;
# -r also the US one for the English data). Nothing copyrighted ships in the repo; the zips
# are for the player's own machines. Produces, in OUTDIR (default dist/):
#   dcb-pc-v<version>-linux.zip     dcb (linux-release) + assets/ + cheats/ + README
#   dcb-pc-v<version>-windows.zip   dcb.exe (windows-cross) + the same payload
# or, with -1 (dcb_asset_ripper embed; src/vfs/payload.hpp), one program each that unpacks the
# same bundle next to itself on its first start:
#   dcb-pc-v<version>-linux         dcb-pc-v<version>-windows.exe
# Bundle layout, all the game reads (found next to the binary, whatever the working dir):
#   assets/<serial>.pak          textures, sound effects, movies
#   assets/<serial>/             English data: fonts, names, text/ catalog, files/
#   assets/dump/<serial>/        the game data (the player's dump)
#
# The version is the CMake project VERSION (x.y.z); the window title carries it too.
set -euo pipefail
cd "$(dirname "$0")"

SERIAL="SLPS-03101"
US_SERIAL="SLUS-01328"
OUTDIR="dist"
REBUILD=0
ONEFILE=0
while getopts "s:o:r1h" opt; do
    case "$opt" in
        s) SERIAL="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        r) REBUILD=1 ;;
        1) ONEFILE=1 ;;
        *) sed -n '2,27p' "$0"; exit 2 ;;
    esac
done

# A dump: assets/dump/<serial>/ (dcb --import), or the older extracted/<serial>/. $2 is the file
# that must be there: layout.txt for the one the game runs, fs for one only the tools read.
dump_dir() {
    local d
    for d in "assets/dump/$1" "extracted/$1"; do
        if [[ -e "$d/$2" ]]; then echo "$d"; return 0; fi
    done
    return 1
}

VERSION="$(grep -m1 -A2 '^project(' CMakeLists.txt | grep -o '[0-9][0-9.]*' | head -1)"
[[ -n "$VERSION" ]] || { echo "error: cannot read project VERSION from CMakeLists.txt" >&2; exit 1; }
JP_DUMP="$(dump_dir "$SERIAL" layout.txt)" || { echo "error: import the $SERIAL dump first (dcb --import)" >&2; exit 1; }
if (( ! ONEFILE )); then
    command -v zip >/dev/null || { echo "error: zip is not installed" >&2; exit 1; }
fi
ASSETS="assets/$SERIAL"
if (( ! REBUILD )) && [[ ! -f "assets/$SERIAL.pak" ]]; then
    echo "error: no assets/$SERIAL.pak to ship; run with -r to build one" >&2
    exit 1
fi

echo "== pack v$VERSION ($SERIAL) =="

echo "-- build: linux-release"
cmake --preset linux-release -DDCB_GAME_ID="$SERIAL" >/dev/null
cmake --build --preset linux-release --target dcb dcb_asset_ripper 2>&1 | tail -1

echo "-- build: windows-cross"
cmake --preset windows-cross -DDCB_GAME_ID="$SERIAL" >/dev/null
cmake --build --preset windows-cross --target dcb 2>&1 | tail -1
RIPPER="$PWD/build/linux-release/dcb_asset_ripper"

# SRC_PAK / SRC_ASSETS: what gets shipped.
SRC_PAK="assets/$SERIAL.pak"
SRC_ASSETS="$ASSETS"
if (( REBUILD )); then
    # A scratch project root: its own assets/, the dumps linked read-only. Every tool writes
    # under it (the ripper's -o, the swap's and rip_movies' --root, en_text's --out).
    WORK="build/pack-work"
    rm -rf "$WORK"
    mkdir -p "$WORK/assets/$SERIAL" "$WORK/extracted"
    WORK="$(cd "$WORK" && pwd)"
    ln -s "$PWD/$JP_DUMP" "$WORK/extracted/$SERIAL"
    # Inputs kept with the loose assets: community fixes (en_text), hand-edited art, the
    # US movie override (rip_movies).
    for d in fixes custom; do
        if [[ -d "$ASSETS/$d" ]]; then cp -r "$ASSETS/$d" "$WORK/assets/$SERIAL/"; fi
    done
    if [[ -d "$ASSETS/disc" ]]; then ln -s "$PWD/$ASSETS/disc" "$WORK/assets/$SERIAL/disc"; fi
    CONVERTED="$WORK/assets/converted/$SERIAL"

    echo "-- assets (rebuild in $WORK): rip + custom art + English data + movies + pack"
    "$RIPPER" unpack "$JP_DUMP" -o "$WORK/assets" >/dev/null
    "$RIPPER" sfx "$WORK/assets/raw/$SERIAL" -o "$WORK/assets" --game "$SERIAL" >/dev/null
    # The rip holds JP art only: put hand-edited images (assets/<serial>/custom/textures/
    # mirrors converted/<serial>/textures/, e.g. the English title) and sprites.txt over it.
    if [[ -d "$ASSETS/custom/textures" ]]; then
        cp -r "$ASSETS/custom/textures/." "$CONVERTED/textures/"
        echo "   custom images: $(find "$ASSETS/custom/textures" -type f | wc -l)"
    fi
    if [[ -f "$ASSETS/custom/sprites.txt" ]]; then cp "$ASSETS/custom/sprites.txt" "$CONVERTED/"; fi
    if US_DUMP="$(dump_dir "$US_SERIAL" fs)"; then
        ln -s "$PWD/$US_DUMP" "$WORK/extracted/$US_SERIAL"
        "$RIPPER" unpack "$US_DUMP" -o "$WORK/assets" >/dev/null
        python3 tools/text/en_text.py --jp "$JP_DUMP" --us "$US_DUMP" --out "$WORK/assets/$SERIAL" >/dev/null
        # The US art over the JP rip; its summary is shown so a failed swap is not missed.
        python3 tools/assets/swap_us_images.py --apply --root "$WORK" 2>&1 | grep -E "^replaced|error" || true
        swapped="$(grep -o 'us/[0-9a-f]*\.raw' "$CONVERTED/assets_manifest.json" | wc -l)"
        (( swapped > 0 )) || { echo "error: the US art swap replaced nothing" >&2; exit 1; }
        echo "   US art: $swapped manifest entries"
    else
        # The English text and art come from it: without it the rebuild would be Japanese.
        echo "error: -r needs the $US_SERIAL dump (assets/dump/ or extracted/) for the English data" >&2
        exit 1
    fi
    # Movies go into converted/ before the pack: the game plays movie/movie<N>.mpg from the pak.
    python3 tools/disc/rip_movies.py --serial "$SERIAL" --root "$WORK" >/dev/null
    "$RIPPER" pack "$CONVERTED" "$WORK/assets/$SERIAL.pak"
    SRC_PAK="$WORK/assets/$SERIAL.pak"
    SRC_ASSETS="$WORK/assets/$SERIAL"
else
    echo "-- assets: shipping the working copy ($SRC_PAK, $SRC_ASSETS/)"
fi
[[ -d "$SRC_ASSETS/text" ]] || echo "   warning: no text/ catalog: menus and dialogs stay Japanese" >&2

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$OUTDIR" "$STAGE/linux" "$STAGE/windows"
OUTDIR="$(cd "$OUTDIR" && pwd)"
cat > "$STAGE/README.txt" <<EOF
Digimon Digital Card Battle PC v$VERSION ($SERIAL)
=================================================

Run dcb (Linux) or dcb.exe (Windows). No BIOS, disc, or import step needed:
everything the game reads is in assets/ next to the program (the single-file
download unpacks it there on its first start):

  assets/$SERIAL.pak       textures, sound effects, movies
  assets/$SERIAL/          English data (fonts, names, text catalog, files)
  assets/dump/$SERIAL/     the game data (your own dump)

Cheats live in cheats/$SERIAL.txt (F4 in game); saves in saves/$SERIAL/.
Settings (window size, keys, gamepad, volume) are written to settings.ini on
first run.
EOF
for OS in linux windows; do
    # Only what the game reads: the pak (the one texture/movie/sfx source it mounts), the
    # loose English files, and the dump. Not converted/, raw/, backup/, movie_src/, disc/
    # (the movies are in the pak), fixes/ or custom/ (already applied).
    A="$STAGE/$OS/assets"
    mkdir -p "$A/$SERIAL" "$A/dump"
    cp "$SRC_PAK" "$A/$SERIAL.pak"
    for f in en_font.bin en_bigfont.bin en_names.txt; do
        if [[ -f "$SRC_ASSETS/$f" ]]; then cp "$SRC_ASSETS/$f" "$A/$SERIAL/"; fi
    done
    for d in files text; do
        if [[ -d "$SRC_ASSETS/$d" ]]; then cp -r "$SRC_ASSETS/$d" "$A/$SERIAL/"; fi
    done
    # -L: the dump may be reached through a symlink (assets/dump -> extracted/).
    cp -rL "$JP_DUMP" "$A/dump/$SERIAL"
    cp "$STAGE/README.txt" "$STAGE/$OS/"
    # cheats/<serial>.txt bootstrap (the trainer saves back to it).
    mkdir -p "$STAGE/$OS/cheats"
    if [[ -f "cheats/$SERIAL.txt" ]]; then
        cp "cheats/$SERIAL.txt" "$STAGE/$OS/cheats/$SERIAL.txt"
    else
        cp docs/cheats.example.txt "$STAGE/$OS/cheats/$SERIAL.txt"
    fi
done
if (( ONEFILE )); then
    # The bundle tree (assets/, cheats/, README.txt) appended to each binary.
    LINUX_ONE="$OUTDIR/dcb-pc-v$VERSION-linux"
    WIN_ONE="$OUTDIR/dcb-pc-v$VERSION-windows.exe"
    "$RIPPER" embed build/linux-release/dcb "$STAGE/linux" "$LINUX_ONE"
    "$RIPPER" embed build/windows-cross/dcb.exe "$STAGE/windows" "$WIN_ONE"
    ls -la "$LINUX_ONE" "$WIN_ONE"
    echo "== packed v$VERSION (single file) =="
    exit 0
fi
cp build/linux-release/dcb "$STAGE/linux/"
cp build/windows-cross/dcb.exe "$STAGE/windows/"

LINUX_ZIP="$OUTDIR/dcb-pc-v$VERSION-linux.zip"
WIN_ZIP="$OUTDIR/dcb-pc-v$VERSION-windows.zip"
rm -f "$LINUX_ZIP" "$WIN_ZIP"  # zip adds to an existing archive: start clean
(cd "$STAGE/linux" && zip -q -9 -r "$LINUX_ZIP" dcb README.txt assets cheats)
(cd "$STAGE/windows" && zip -q -9 -r "$WIN_ZIP" dcb.exe README.txt assets cheats)
ls -la "$LINUX_ZIP" "$WIN_ZIP"
echo "== packed v$VERSION =="
