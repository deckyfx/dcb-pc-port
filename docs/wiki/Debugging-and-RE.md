# Debugging and reverse engineering

[Home](Home.md)

The full reverse-engineering loop (play → trace → document → poke) is in
[RE_WORKFLOW.md](../RE_WORKFLOW.md); subsystem notes are in [docs/re/](../re/README.md). For the
Ghidra side see [Ghidra](Ghidra.md).

## Input debugging

`DCB_TRACE_PAD=<n>` logs the first *n* controller-port register accesses. `DCB_TRACE_INPUT=1` logs
key presses and the pad state they produce, and what the game's pad library last read over the
port.

`DCB_PAD_SCRIPT="<from>-<to>:<Button>[+<Button>],..."` holds pad buttons during those frames (names
as in `settings.ini`; `Any` = some unbound key), e.g. `DCB_HEADLESS=1 DCB_PAD_SCRIPT=2000-2000:Any`
skips the opening movie in a headless run.

## Input record / replay

`DCB_RECORD=<file>` logs the pad state the game is given each frame (after keyboard, gamepad,
`DCB_PAD_SCRIPT` and the movie-skip tap are combined), plus the "any key" pulse.
`DCB_REPLAY=<file>` feeds such a log back: host input is ignored until the log ends, then control
returns to the keyboard/gamepad; add `DCB_REPLAY_EXIT=1` to quit at that point instead.

Guest time is virtual, so a replay reproduces the run frame for frame (compare `DCB_SNAPSHOT`
output to check). The file is a small text log (`src/platform/input_log.hpp`) that stores only
changes, starts with a `DCB-INPUT <version> <game id>` header (logs for another version or game are
rejected) and is flushed about once a second, so a crash still leaves a usable file. Both variables
can be combined to re-record a replay.

## Scripted save-state checks

- `DCB_STATE_SAVE_AT=<frame>[,...]` / `DCB_STATE_LOAD_AT=<frame>[,...]` save and load the selected
  slot after that many frames.
- `DCB_EXIT_AT=<frame>` quits cleanly.
- `DCB_STATE_STRESS=<n>` saves, runs *n* frames, loads and runs them again, and aborts if the
  machine differs (every frame boundary with *n=1*).
- `DCB_RESET_AT=<frame>[,...]` resets the game to its power-on state at that frame.

Frame numbers count every frame run, as the `DCB_SNAPSHOT` file names do: after a load at *M* of a
state saved at *N*, snapshot *M+k* equals snapshot *N+k* of a run without the load. With
`DCB_RECORD`, loading a state rewinds the recording to the loaded frame, so the log replays the
timeline that was finally played.

`DCB_STATE_DUMP_AT=<frame>` + `DCB_STATE_DUMP_PATH=<file>` writes the slot bytes for offline
analysis (`tools/re/scan_stacks.py` classifies host pointers on game stacks). Why states cannot be
kept across runs: [Playing](Playing.md#persistent-save-states).

## Reverse-engineering aids

- `DCB_WATCH=800E0000-800E1800` (or `./dcb.sh -W 800E0000-800E1800`) logs every write the game
  makes into those RAM ranges, with the old and new value, the function that wrote it and its
  callers: do something in the game (win a card, level up) and read which address changed and which
  code changed it.
- `DCB_WATCH_BATTLE=1`: when a battle starts, points the RAM write watch at both players' battle
  data and the battle state struct, so a played round logs which fields change and who writes them
  (see [docs/re/battle.md](../re/battle.md)).
- `DCB_COVERAGE=<file>` writes per-function call counts (address, overlay, calls, first frame) at
  exit; `DCB_TRACE_CALLS=<n>` logs the first *n* calls live.
- `tools/re/coverage_diff.py a.json b.json` diffs two coverage runs.
- `DCB_LOG_LOADS=1` logs named asset loads (disc files, SPU uploads, XA streams, MDEC decodes) with
  frame numbers.

## Environment variable reference

Everything the runtime reads from the environment, with the page that explains it:

| Variable | Effect |
|---|---|
| `DCB_DISC=<path>` | game data or disc image to run ([Game Data](Game-Data.md#where-data-is-found)) |
| `DCB_HEADLESS=1` | run without a window |
| `DCB_IMPORT_IMAGE=<path>` | first-run import without the explanation and dialog ([Game Data](Game-Data.md#first-run)) |
| `DCB_SETTINGS=<file>` | `settings.ini` to use ([Playing](Playing.md#settings)) |
| `DCB_FILTER=linear\|nearest`, `DCB_SCALE=fit\|integer` | override the display filter / scale mode for this run |
| `DCB_CHEATS=<file>` | cheat file ([Trainer](Trainer.md#cheat-file)) |
| `DCB_TRACE_CHEATS=1` | log each frame's cheat writes |
| `DCB_LANG=<lang>` | text catalog language, default `en` ([English Text](English-Text.md#behaviour)) |
| `DCB_TRACE_TEXT=1\|hex` | log strings drawn by the text engine |
| `DCB_HD_PACK=<.pak\|folder>`, `DCB_HD_MANIFEST=<file>` | replacement textures ([Textures](Textures.md#replacement-textures)) |
| `DCB_LOG_HD=1`, `DCB_TRACE_HD=<n>`, `DCB_LOG_TEX=1`, `DCB_TRACE_PRIMS=1` | texture diagnostics ([Textures](Textures.md)) |
| `DCB_TRACE_MOVIE=1` | native movie timing ([Movies](Movies.md)) |
| `DCB_LOG_FILES=1`, `DCB_CD_FILES=1` | file access log / original CD path ([Game Data](Game-Data.md#native-file-access)) |
| `DCB_LOG_LOADS=1` | named asset loads with frame numbers |
| `DCB_TRACE_BIOS=1` | trace every BIOS call (`./dcb.sh -t`) |
| `DCB_TRACE_CD=1` | trace the CD-ROM controller and CD DMA |
| `DCB_TRACE_PAD=<n>`, `DCB_TRACE_INPUT=1`, `DCB_PAD_SCRIPT=...` | input debugging (above) |
| `DCB_RECORD=<file>`, `DCB_REPLAY=<file>`, `DCB_REPLAY_EXIT=1` | input record / replay (above) |
| `DCB_SNAPSHOT=<dir>` | write the displayed frame as `frame_NNNNN.ppm` every 30 frames |
| `DCB_SNAPSHOT_VRAM=1` | with `DCB_SNAPSHOT`, also the whole 1024x512 VRAM (`vram_NNNNN.ppm`) |
| `DCB_AUDIO_DUMP=<file>` | raw s16le stereo 44100 Hz of everything played (`ffmpeg -f s16le -ar 44100 -ac 2`) |
| `DCB_FAST=1` | run unpaced (no frame-rate throttling) |
| `DCB_WATCHDOG=<seconds>` | abort after that many seconds (not on Windows) |
| `DCB_STATE_SAVE_AT`, `DCB_STATE_LOAD_AT`, `DCB_EXIT_AT`, `DCB_RESET_AT`, `DCB_STATE_STRESS`, `DCB_STATE_DUMP_AT`, `DCB_STATE_DUMP_PATH` | scripted save-state checks (above) |
| `DCB_WATCH=<ranges>`, `DCB_WATCH_BATTLE=1`, `DCB_COVERAGE=<file>`, `DCB_TRACE_CALLS=<n>` | reverse-engineering aids (above) |
