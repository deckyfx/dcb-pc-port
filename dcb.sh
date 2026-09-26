#!/usr/bin/env bash
# Build and run the native game in one step.
#
#   ./dcb.sh             build (Debug) and run
#   ./dcb.sh -r          regenerate C from MIPS first (after recompiler/config changes)
#   ./dcb.sh -t          trace every BIOS call (DCB_TRACE_BIOS=1)
#   ./dcb.sh -g          run under gdb; stops where the game hits something unimplemented
#   ./dcb.sh -b          build only, don't run
#   ./dcb.sh -w          also cross-build the Windows .exe
#   ./dcb.sh -s SERIAL   game id (default SLPS-03101)
# Flags combine: ./dcb.sh -r -t
set -euo pipefail
cd "$(dirname "$0")"

SERIAL="SLPS-03101"
RECOMPILE=0 TRACE=0 GDB=0 RUN=1 WINDOWS=0
while getopts "rtgbws:h" opt; do
    case "$opt" in
        r) RECOMPILE=1 ;;
        t) TRACE=1 ;;
        g) GDB=1 ;;
        b) RUN=0 ;;
        w) WINDOWS=1 ;;
        s) SERIAL="$OPTARG" ;;
        *) sed -n '2,11p' "$0"; exit 2 ;;
    esac
done

[[ -f "extracted/$SERIAL/exe/boot.exe" ]] || {
    echo "error: extracted/$SERIAL missing. Run: python3 tools/disc/extract_disc.py <cue> -o extracted/$SERIAL" >&2
    exit 1
}

[[ -f build/linux-debug/build.ninja ]] || cmake --preset linux-debug -DDCB_GAME_ID="$SERIAL"
if (( RECOMPILE )); then
    cmake --build --preset linux-debug --target recompile
fi
cmake --build --preset linux-debug

if (( WINDOWS )); then
    [[ -f build/windows-cross/build.ninja ]] || cmake --preset windows-cross -DDCB_GAME_ID="$SERIAL"
    cmake --build --preset windows-cross
    echo "windows: build/windows-cross/dcb.exe"
fi

(( RUN )) || exit 0
(( TRACE )) && export DCB_TRACE_BIOS=1
mkdir -p logs
if (( GDB )); then
    exec gdb -q -ex run -ex bt --args ./build/linux-debug/dcb "extracted/$SERIAL/exe/boot.exe"
fi
# Keep a copy of every run in logs/run.log while still printing to the terminal.
set +e
./build/linux-debug/dcb "extracted/$SERIAL/exe/boot.exe" 2>&1 | tee logs/run.log
status=${PIPESTATUS[0]}
(( status == 134 )) && echo "-- stopped at an unimplemented feature (see last lines above) --"
exit "$status"
