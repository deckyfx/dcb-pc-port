# RE workflow: play → trace → document → poke

Reverse-engineering by feature, not by whole game. Each milestone traces
exactly what it needs, names it, and pokes it. Findings accumulate in
`ghidra/symbols/` and `docs/re/`.

## 1. Play (deterministic reproduction)

Runs are bit-identical run to run. The pieces:

| Tool | Purpose |
|---|---|
| `DCB_HEADLESS=1 DCB_FAST=1` | no window, unthrottled (scripted runs) |
| `DCB_PAD_SCRIPT="<from>-<to>:<Button>,..."` | hold buttons on frames (e.g. `600-610:Start`) |
| `DCB_RECORD=<file>` / `DCB_REPLAY=<file>` (+ `DCB_REPLAY_EXIT=1`) | record and replay host input |
| Save states: F5/F7, `DCB_STATE_SAVE_AT` / `LOAD_AT` | rewind to just before the interesting call |
| `DCB_EXIT_AT=<frame>` | quit cleanly after N frames |
| `DCB_SNAPSHOT=<dir>` | frame PPMs every 30 frames; compare hashes across runs |

## 2. Trace

### 2.1 Function coverage

Every generated function calls `PSX_COVER(id)` from its prologue
(`src/runtime/include/psx/recomp.h`). Off by default: one predictable branch
on `psx_coverage_armed`, so guest state and timing are bit-identical.

| Variable | Effect |
|---|---|
| `DCB_COVERAGE=<file>` | write JSON at exit (and on fatal errors, not on `abort()` traps) |
| `DCB_TRACE_CALLS=<n>` | log the first *n* calls live to stderr (`[call] frame N ADDR symbol`) |

ids are dense in recompiler emit order (boot EXE, then overlays);
`psx_coverage_names[]` in `generated/<id>/function_table.c` maps each id to
`(addr, overlay, symbol)`. Report schema: `{"version":1,"count":N,"entries":
[{"id","addr":"0x…","overlay":"","symbol":"…","calls","first_frame"}]}`.
`first_frame` is the host frame (same numbering as `DCB_SNAPSHOT`).

```sh
DCB_HEADLESS=1 DCB_FAST=1 DCB_EXIT_AT=600 DCB_COVERAGE=/tmp/dcb-re-tracing/a.json ./build/linux-debug/dcb extracted/SLPS-03101
python3 tools/re/coverage_diff.py /tmp/dcb-re-tracing/a.json /tmp/dcb-re-tracing/b.json
```

`coverage_diff.py` matches by `(overlay, addr)` and reports: only in B, only
in A, changed counts (default: ≥10 calls both sides, ratio ≥2.0).

### 2.2 Named asset-load log

`DCB_LOG_LOADS=1` prints one line per event with frame + guest cycle:

```text
[load] frame 8 spu 0x67FC0 96 KB
[load] frame 20 file A:\SE1.PAK 285 KB
[load] frame 26 file B:\CARD2.CDD 80 KB
[load] frame 97 stream DIGIMON.MOV.raw2352 from lba 105731
```

- **Game files**: the native file layer (`src/game/overrides/files.cpp`) serves
  every data file without the CD drive and logs it by game path (`file X:\…`,
  with `(loose file)` when it came from `assets/<id>/files/`).
- **Streams**: a ReadS (movie video + XA audio) logs one `stream` line where it
  starts, not every sector.
- **Other CD reads → files** via `extracted/<id>/layout.txt` (falls back to bare
  LBAs on raw images); contiguous sectors coalesce into one line. With
  `DCB_CD_FILES=1` (original CD file path) data loads show up here instead.
- **DRV entries**: resolve `lba → file offset → entry` with
  `tools/disc/drv_unpack.py` (the log gives file + LBA).
- **SPU**: DMA channel 4 uploads coalesced per contiguous sound-RAM run
  (`spu ADDR SIZE`). Match against `assets/converted/<id>/sfx_manifest.json`.
- **XA**: stream start/stop edges with file + channel.
- **MDEC**: decode start/stop edges (movie segments in `DIGIMON.MOV`).
- **Textures**: `DCB_LOG_HD` / `DCB_TRACE_HD` (existing).

### 2.3 Ghidra bridge

| Direction | Tool |
|---|---|
| Ghidra → repo | `ghidra/scripts/export_symbols.py` → `ghidra/symbols/<serial>.json` (user-named functions only, sorted, diffable) |
| Repo → Ghidra | `ghidra/scripts/import_symbols.py` (renames `FUN_*` only, never overwrites human names; re-runnable) |
| Coverage → Ghidra | `ghidra/scripts/ImportCoverage.java` (tags `DCB_COV_EXECUTED`, bookmarks, call-count + first-frame comment; clears its own previous run) |

The recompiler and the trace tooling read `ghidra/symbols/` names; coverage
JSON carries `(addr, overlay, symbol)` so the diff works without Ghidra open.

## 3. Document

- Export new names to `ghidra/symbols/<serial>.json` (reviewed, not raw dumps).
- One page per subsystem under `docs/re/` (see `docs/re/README.md`):
  addresses, resources, verification (diff excerpt, log excerpt, poke).
- The HYBRID_EN_ASSETS.md §§4–6 are three draft pages (text engine, card DB,
  graphics); promote them as the features land.

## 4. Poke

- **Trainer** (F4, `cheats/<serial>.txt`, `DCB_CHEATS`): RAM writes/freeze +
  memory search. Fastest way to test "is this address the thing I think".
- **C overrides** (`config/<serial>/overrides.json` + `src/game/overrides/`):
  replace a recompiled function natively. Start log-and-call-through
  (the original still compiles as `f_<addr>`), then change behavior.
- **Save states** to retry in seconds: save just before the call, poke, load,
  repeat.

## Worked example: what the movie skip changes

(record → replay twice ± action → diff → Ghidra → name → poke)

Boot shape, from the load log: SPU banks at frame 8, A.DRV streaming from 24,
DIGIMON.MOV from 245 (MDEC start 249), game code from ~5069, MMM.DAT (menu
data) at ~19700. The opening movie runs past frame 26000 unskipped.

```sh
# 1. Two 3000-frame runs: plain boot vs Start-tap at frame 400 (skips the movie).
DCB_HEADLESS=1 DCB_FAST=1 DCB_EXIT_AT=3000 \
  DCB_COVERAGE=/tmp/dcb-re-tracing/cov-menu.json ./build/linux-debug/dcb extracted/SLPS-03101
DCB_HEADLESS=1 DCB_FAST=1 DCB_EXIT_AT=3000 DCB_PAD_SCRIPT=400-406:Start \
  DCB_COVERAGE=/tmp/dcb-re-tracing/cov-skip.json ./build/linux-debug/dcb extracted/SLPS-03101
# 2. Diff: the skip drops two functions to zero calls.
python3 tools/re/coverage_diff.py /tmp/dcb-re-tracing/cov-menu.json /tmp/dcb-re-tracing/cov-skip.json
#   0x80068134  f_80068134  (A=30 -> B=0 down)
#   0x80067778  f_80067778  (A=30 -> B=0 down)
#   The load log agrees: MDEC start at 249, stop at 251 in the skip run.
# 3. Import the plain run into Ghidra (ImportCoverage.java): both functions
#    are tagged with first_frame=8, i.e. they run from boot and keep running
#    per frame while the movie plays. Name them (e.g. mdec_movie_pump and
#    mdec_movie_feed — verify in the disassembly first), export back to
#    ghidra/symbols/SLPS-03101.json.
# 4. Poke: override one log-and-call-through, or trainer-freeze its state,
#    and watch the movie stall or the skip break.
```

Next worked example (menu confirm) follows the same pattern once the menu
frame is pinned: replay twice with/without a Cross press after MMM.DAT
loads (~19700), diff, name, poke.
