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
#   ./dcb.sh -H          log each replacement texture as it is used (DCB_LOG_HD=1)
#   ./dcb.sh -T          log every file load and texture upload, with its source file (DCB_LOG_LOADS=1 DCB_LOG_TEX=1)
#   ./dcb.sh -W RANGES   log the game's writes to RAM ranges, e.g. -W 800E0000-800E1800 (DCB_WATCH)
#   ./dcb.sh -G          open Ghidra (ghidra/ghidra_12.1.2_PUBLIC, MCP scripting on) instead
# Flags combine: ./dcb.sh -r -t
set -euo pipefail
cd "$(dirname "$0")"

SERIAL="SLPS-03101"
RECOMPILE=0 TRACE=0 GDB=0 RUN=1 WINDOWS=0 LOG_HD=0 LOG_TEX=0 WATCH=""
while getopts "rtgbws:GHTW:h" opt; do
    case "$opt" in
        r) RECOMPILE=1 ;;
        t) TRACE=1 ;;
        g) GDB=1 ;;
        b) RUN=0 ;;
        w) WINDOWS=1 ;;
        s) SERIAL="$OPTARG" ;;
        G) exec tools/ghidra/ghidra_gui.sh ;;
        H) LOG_HD=1 ;;
        T) LOG_TEX=1 ;;
        W) WATCH="$OPTARG" ;;
        *) sed -n '2,15p' "$0"; exit 2 ;;
    esac
done

[[ -d "disc/$SERIAL" ]] || {
    echo "error: put the disc image (.cue + .bin) in disc/$SERIAL/" >&2
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
(( LOG_HD )) && export DCB_LOG_HD=1
(( LOG_TEX )) && export DCB_LOG_LOADS=1 DCB_LOG_TEX=1
[[ -n "$WATCH" ]] && export DCB_WATCH="$WATCH"
mkdir -p logs
if (( GDB )); then
    exec gdb -q -ex run -ex bt --args ./build/linux-debug/dcb
fi
# Keep a copy of every run in logs/run.log while still printing to the terminal.
set +e
./build/linux-debug/dcb 2>&1 | tee logs/run.log
status=${PIPESTATUS[0]}
(( status == 134 )) && echo "-- stopped at an unimplemented feature (see last lines above) --"
exit "$status"
