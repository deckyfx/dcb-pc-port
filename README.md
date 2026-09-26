# dcb-static-recomp

Static recompilation of **Digimon World: Digital Card Arena** (PS1, SLPS-03101) into a native PC
executable: MIPS R3000A → C, with native HLE of the kernel and Psy-Q libraries. It needs
**no BIOS and no emulator** at runtime.

> Copyrighted inputs (`disc/`, `bios/`, `extracted/`, `assets/`, `generated/`) are gitignored. Never commit them.

## Showcase

![Title screen running natively (Linux, SDL3 on Wayland)](screenshoots/00_title_screen.png)

*Title screen running natively on Linux (SDL3 on Wayland): recompiled game code, native GPU renderer.*

## Progress

- [x] Disc extraction and Ghidra pipeline (`ghidra_psx_ldr`, Psy-Q signatures, Ghidra MCP)
- [x] MIPS → C recompiler with overlay support (boot EXE 99.4% covered, overlays 83–98%)
- [x] Native BIOS/kernel HLE: no BIOS image needed
- [x] Game task system on native fibers
- [x] CD-ROM streaming from the original disc image
- [x] Render boot FMV (MDEC, 24-bit)
- [x] Title screen (GPU renderer, SDL3 window)
- [x] Sound: SPU music and effects, XA-ADPCM movie audio
- [x] Runs from the disc image alone (no BIOS, no extracted files)
- [x] Runs from extracted game data alone (sectors rebuilt from files; verified identical to the disc)
- [x] Input: keyboard and gamepad → PS1 digital pad (timed SIO0 model); any key skips movies
- [x] Main menu and navigation (title, main menu, Reception, Deck screens)
- [x] Card battles (KAWSEG overlay): a full battle played through
- [ ] Remaining game modes and overlays (EVOSEG, SAISEG, SUBSEG, SUGSEG, ENDSEG)
- [x] Memory card saves verified in game (`saves/<serial>/card1.mcd`, raw 128 KB `.mcd` image)
- [ ] `PSX2.EXE` mode (`LoadExec`)
- [x] One-time asset import from the player's own dump: no disc needed afterwards, no copyrighted data in the download (`dcb --import`, or a file picker on first run)
- [ ] Windows x64 release build tested on Windows
- [ ] English build: JP code + English assets from the player's US dump (SLUS-01328), research in progress
- [x] PC options: `settings.ini` (window scale, filtering, aspect, key/gamepad rebinding, volume); resizable window, picture fits it (F8: fit / integer)
- [x] Performance overlay (FPS, game FPS, CPU/GPU load, audio queue): F3
- [x] Host-driven main loop: the game runs on fibers; pause (P), frame advance (N), fast-forward (hold Tab) ([design](docs/HOST_MAIN_LOOP.md))
- [x] Input record / replay (`DCB_RECORD`, `DCB_REPLAY`): reproducible runs, bit-identical frames
- [x] GTE commands implemented (unit-tested; awaiting in-game use)
- [x] Memory card file API (`bu00:`) implemented and used by the game's saves
- [x] CI: Linux tests + Windows .exe (manual trigger for now)
- [ ] Enhance / upscale assets
- [ ] Enhancements: widescreen, translation
- [ ] Save states (within a run)
- [ ] Trainer: cheat codes, memory search
- [ ] Network Battle
- [ ] Custom Battle mode: pick the opponent and the arena
- [ ] Rust port of the game logic

## Layout

```
disc/<serial>/          disc images (.bin/.cue), one folder per serial       [ignored]
bios/                   retail BIOS dumps: reference and diff-testing only   [ignored]
extracted/<serial>/     imported game data: layout.txt, iso_meta.bin, fs/    [ignored]
                        (extract_disc.py adds exe/boot.*, manifest for dev)
assets/raw|converted/   asset pipeline in/out                                [ignored]
generated/<serial>/     MIPS→C output of tools/recomp; never hand-edited     [ignored]
config/<serial>/        recompiler inputs: functions.json, overlays, overrides
ghidra/project/         Ghidra DB (DCB.gpr; one folder per serial)           [ignored]
ghidra/scripts/         Ghidra scripts (export_functions.py → config/)
ghidra/symbols/         exported, reviewable symbol maps
src/runtime/            CPU context, memory bus, dispatch, GTE (the C ABI of generated code)
src/hle/                kernel + Psy-Q replacements, MMIO fallback (see src/hle/README.md)
src/platform/           host seam: SDL3 window, input, audio (+ headless)
src/game/               entry point + hand-written overrides of recompiled functions
tools/disc/             extract_disc.py (+ tests), verify_import.sh
tools/ghidra/           setup_ghidra_mcp.sh, import_ghidra.sh
tools/recomp/           the MIPS→C recompiler (C++ host tool)
tools/assets/           TIM / VAB / XA / STR converters
tests/                  runtime unit tests (ctest)
```

## Workflow

```bash
# 1. Disc → filesystem + boot EXE (dev tool: also writes exe/boot.* for Ghidra and a manifest;
#    players use the native importer instead, see "Game data" below — same layout.txt/fs/iso_meta.bin)
python3 tools/disc/extract_disc.py disc/SLPS-03101/dcb_jp.cue -o extracted/SLPS-03101

# 2. Boot EXE → Ghidra (ghidra_psx_ldr loader + Psy-Q signatures), ~3 min
tools/ghidra/import_ghidra.sh SLPS-03101

# 3. Overlay table (P.DRV) → config/<serial>/overlays.json
python3 tools/disc/pdrv_segments.py SLPS-03101

# 4. Recompile MIPS → C into generated/SLPS-03101/ (+ discovered.json), then build.
#    Discovery: entry + Ghidra functions + every call (EXE and overlays) + data pointers + lui/addiu
#    constants, followed by control flow; jump tables are sized from their sltiu bounds check.
cmake --preset linux-debug && cmake --build --preset linux-debug --target recompile

# 5. Mirror the discovery into Ghidra (new functions + overlay blocks such as KAWSEG::801E2A6C):
#    GUI: Script Manager > DCB > ApplyDiscovered.java (or via MCP run_ghidra_script), or headless
#    with Ghidra closed:
../ghidra_12.1.2_PUBLIC/support/analyzeHeadless ghidra/project DCB/SLPS-03101 -process SLPS_031.01 \
    -noanalysis -scriptPath ghidra/scripts -postScript ApplyDiscovered.java "$PWD"

# 6. Build: native dev loop, or a Windows x64 .exe cross-compiled from Linux
cmake --preset linux-debug   && cmake --build --preset linux-debug && ctest --preset linux-debug
cmake --preset windows-cross && cmake --build --preset windows-cross
./build/linux-debug/dcb            # or ./dcb.sh
```

**Game data.** The program ships without any game data. The player imports a dump of their own
disc once, natively (no Python needed):

```bash
dcb --import <disc.cue|disc.bin> [dest-dir] [--force]   # default dest-dir: ./extracted
```

This reads a raw `.cue`/`.bin` dump (Mode 2, 2352-byte sectors; a `.bin` without its `.cue` works
too), identifies the game from `SYSTEM.CNF` and writes `extracted/<serial>/`: `layout.txt`,
`iso_meta.bin` and `fs/` (XA/STR files as whole sectors, `*.raw2352`). The output is byte-identical
to `tools/disc/extract_disc.py` (checked by `ctest` on a synthetic disc, and on a real dump with
`tools/disc/verify_import.sh <disc.cue>`). The import goes to a temporary directory that is renamed
into place only when complete, checks free space first, and rejects wrong input with a clear
message: cooked `.iso`/`.chd`/`.pbp`, an audio track, a non-PlayStation disc, a truncated dump, or
another game. Accepted discs: SLPS-03101 (the version this port plays) and SLUS-01328 (kept for a
future "Japanese code + English assets" build). Other serials are refused: this build could not run
them, so importing would only fill the disk.

**First run.** When no game data is found, the SDL build explains what is needed, opens the system
file dialog (`.cue`/`.bin`), imports with a progress window ("Importing… NN%", close it to cancel)
and boots. Picking the US disc imports it and asks again for the Japanese one. Headless runs
(`DCB_HEADLESS=1`) print these instructions and exit with status 1. After the import the disc image
is no longer needed. (`DCB_IMPORT_IMAGE=<path>` skips the explanation and the dialog, for automated
tests of the flow.)

**Where data is found.** `dcb [extracted-dir | disc.cue | disc.bin]`. Without an argument it uses, in
order: `DCB_DISC`, `extracted/<serial>/` (imported data), then a `.cue`/`.bin` in `disc/<serial>/`
or the current directory. Extracted data rebuilds every CD sector on demand, so the game runs from
it alone (frame-for-frame identical to the disc image), and individual files can later be replaced
(translation, enhanced assets).

Select the target with `-DDCB_GAME_ID=SLUS-01328` (default: `SLPS-03101`).

**Settings.** `settings.ini` (display, audio, key/gamepad bindings, hotkeys) is looked up in this
order: `DCB_SETTINGS`, the current directory, next to the executable, then the per-user file in home
(`~/.config/dcb-pc-port/` or `%APPDATA%\dcb-pc-port\`), where it is created on first run if none
exists. Keep one in the project root (gitignored) while developing. Memory-card saves go to
`saves/<serial>/card1.mcd` under the current directory: a raw 128 KB card image that emulators and
card managers also read.

**Input debugging.** `DCB_TRACE_PAD=<n>` logs the first *n* controller-port register accesses.
`DCB_PAD_SCRIPT="<from>-<to>:<Button>[+<Button>],..."` holds pad buttons during those frames (names
as in `settings.ini`; `Any` = some unbound key), e.g. `DCB_HEADLESS=1 DCB_PAD_SCRIPT=2000-2000:Any`
skips the opening movie in a headless run.

**Input record / replay.** `DCB_RECORD=<file>` logs the pad state the game is given each frame
(after keyboard, gamepad, `DCB_PAD_SCRIPT` and the movie-skip tap are combined), plus the
"any key" pulse. `DCB_REPLAY=<file>` feeds such a log back: host input is ignored until the log
ends, then control returns to the keyboard/gamepad; add `DCB_REPLAY_EXIT=1` to quit at that
point instead. Guest time is virtual, so a replay reproduces the run frame for frame (compare
`DCB_SNAPSHOT` output to check). The file is a small text log (`src/platform/input_log.hpp`) that
stores only changes, starts with a `DCB-INPUT <version> <game id>` header (logs for another
version or game are rejected) and is flushed about once a second, so a crash still leaves a
usable file. Both variables can be combined to re-record a replay.

## Ghidra MCP

`.mcp.json` registers the `ghidra` server (bethington/ghidra-mcp 6.0.0, built for Ghidra 12.1.2).
Install or reinstall it with `tools/ghidra/setup_ghidra_mcp.sh`. Start Ghidra with
`tools/ghidra/ghidra_gui.sh`: it launches through PyGhidra (so `.py` scripts work in the GUI) and sets
`GHIDRA_MCP_ALLOW_SCRIPTS=1` (so MCP can run repo scripts; this allows arbitrary Java in Ghidra,
loopback only). Then in Ghidra:
enable **GhidraMCP** under *File → Configure → Configure All Plugins* (once), open the program, and choose
*Tools → GhidraMCP → Start MCP Server*.

## Discs

| Serial | Title | Boot EXE | Entry |
|---|---|---|---|
| SLPS-03101 | Digimon World: Digital Card Arena (JP) | SLPS_031.01 (+ PSX2.EXE) | 0x80058CFC |
| SLUS-01328 | Digimon Digital Card Battle (US) | SLUS_013.28 | 0x80056270 |
