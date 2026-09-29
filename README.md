# dcb-static-recomp

Static recompilation of **Digimon World: Digital Card Arena** (PS1, SLPS-03101) into a native PC
executable: MIPS R3000A → C, with native HLE of the kernel and Psy-Q libraries. It needs
**no BIOS and no emulator** at runtime.

> Copyrighted inputs (`disc/`, `bios/`, `extracted/`, `assets/`, `generated/`) are gitignored. Never commit them.

## Showcase

<table>
  <tr>
    <td align="center" width="33%"><img src="screenshoots/00_title_screen.png" alt="Title screen"><br><sub>Title screen</sub></td>
    <td align="center" width="33%"><img src="screenshoots/01_load_screen.png" alt="Load screen in English"><br><sub>Load screen in English</sub></td>
    <td align="center" width="33%"><img src="screenshoots/02_card_data.png" alt="Card data"><br><sub>Card data</sub></td>
  </tr>
  <tr>
    <td align="center" width="33%"><img src="screenshoots/03_battle_cafe.png" alt="Battle Cafe"><br><sub>Battle Cafe</sub></td>
    <td align="center" width="33%"><img src="screenshoots/04_battle_ui.png" alt="Battle UI"><br><sub>Battle UI</sub></td>
    <td align="center" width="33%"><img src="screenshoots/05_debug_menu.png" alt="Native pause menu"><br><sub>Native pause menu</sub></td>
  </tr>
  <tr>
    <td align="center" width="33%"><img src="screenshoots/06_polygon_battle.png" alt="Polygon battle"><br><sub>Polygon battle</sub></td>
  </tr>
</table>

*Running natively on Linux (SDL3 on Wayland): recompiled game code, native GPU renderer, English
text and art built from the player's own discs.*

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
- [ ] English build: JP code + English assets from the player's US dump (SLUS-01328) ([research and plan](docs/HYBRID_EN_ASSETS.md))
- [x] PC options: `settings.ini` (initial window size, filtering, aspect, key/gamepad rebinding, volume); resizable window, picture fits it (F8: fit / integer)
- [x] Native pause menu: Esc / F1 (gamepad Start+Select) with save/load slots, settings, controls, memory-card backup/restore, about, quit with confirmation
- [x] Performance overlay (FPS, game FPS, CPU/GPU load, audio queue): F3
- [x] Host-driven main loop: the game runs on fibers; pause (P), frame advance (N), fast-forward (hold Tab) ([design](docs/HOST_MAIN_LOOP.md))
- [x] Input record / replay (`DCB_RECORD`, `DCB_REPLAY`): reproducible runs, bit-identical frames
- [x] Save states within a run: F5 save, F7 load, F6 slot 1-4 (Linux, Windows MinGW build)
- [x] GTE commands implemented (unit-tested; awaiting in-game use)
- [x] Memory card file API (`bu00:`) implemented and used by the game's saves
- [x] CI: Linux tests + Windows .exe (manual trigger for now)
- [x] Asset pipeline: rip textures/sound banks, pack them into one `.pak` the game loads (same-size edits today)
- [ ] Enhance / upscale assets (needs a renderer with higher internal resolution to show HD art)
- [ ] Enhancements: widescreen, translation
- [x] Save states within a run: F5 save, F7 load, F6 slot (bit-identical after a load)
- [x] Trainer (F4, or the F1 menu): built-in presets (all cards, all Digi parts), your own GameShark-style codes (`cheats/<serial>.txt`), battle actions on F10/F11/F12, memory search
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
tools/assets/           TIM / VAB / XA / STR converters, swap_us_images.py (+ tests)
tools/text/             en_text.py: English font and card/deck text from the US dump (+ tests)
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
ghidra/ghidra_12.1.2_PUBLIC/support/analyzeHeadless ghidra/project DCB/SLPS-03101 -process SLPS_031.01 \
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

**Trainer (cheats and memory search).** `F4` (`[hotkeys] trainer`), or Trainer in the `F1` menu,
opens a panel over the game, which stays paused while it is open; `F4` or `Esc` closes it, `Tab`
switches between its pages:

- **Presets:** cheats built into the port for this game (all cards ×4, all 127 Digi parts). They
  cannot be edited; their on/off state is saved in the cheat file as `!preset <name> on|off`.
- **Battle:** during a card battle, `F10` (`[hotkeys] battle_p1`) applies the P1 lines that are
  on (HP, circle/triangle/cross attack, DP, each with its value), `F11` (`battle_p2`) the P2
  lines, and `F12` (`battle_reset`) puts every stat they changed back. Values are multiples of 10
  up to the game's caps (9990, DP 90): Left/Right step by 10, PgUp/PgDn by 1000, or type a
  number and press Enter. Two more lines, "P1 / P2 deck in order (no shuffle)", work on their own
  while ticked: that player's deck is never shuffled, so cards are drawn in deck order. Saved in
  the cheat file as `!battle` lines. See [docs/re/battle.md](docs/re/battle.md).
- **Custom:** your own codes from the cheat file below, and what you freeze on the Search page.
- **Search:** memory search, below.

The `F1` menu's Hotkeys page lists every hotkey and what it does.

Cheats live in `cheats/<serial>.txt` (e.g. `cheats/SLPS-03101.txt`), looked up in the current
directory, then next to the executable (`DCB_CHEATS=<file>` overrides); the folder is gitignored
and [`docs/cheats.example.txt`](docs/cheats.example.txt) is a template. The format is a name in
brackets with `on`/`off`, then PS1 GameShark / Action Replay lines:

```
# comment
[Infinite money] on
800B1234 270F      ; 16-bit write
```

Supported code types: `80` / `30` (16 / 8-bit write), `10` / `11` and `20` / `21` (16 / 8-bit
increment / decrement), `D0`–`D3` and `E0`–`E3` (16 / 8-bit ==, !=, <, > conditions on the next
line; consecutive conditions must all hold), `C0` (gate the rest of the cheat) and `50` (serial
repeater). `C1`, `C2`, `D4`–`D6`, `1F` and any other type are rejected with a message and the
cheat is never half-applied. Enabled cheats are written once per frame at the frame boundary.
`DCB_TRACE_CHEATS=1` logs each frame's writes (and how many bytes the game had changed back).

- *Cheats page*: `Up`/`Down` select, `Enter`/`Space` on/off, `Del` remove, `R` reload the file,
  `S` save it (toggles, removals and frozen values; your comments are kept).
- *Search page*: pick the value size (8/16/32-bit) and signed or unsigned with `Left`/`Right`, type a
  value (decimal, `-5`, `0x1F` or `$1F`), pick a filter (`= != > <` value, or `changed`,
  `unchanged`, `increased`, `decreased` since the last filter) and press `Enter`. Workflow: search
  the current amount, close the panel, let it change in game, reopen and filter again until a few
  addresses remain. On a result, `F` freezes it (adds an enabled cheat holding the typed value, or
  the current one if the field is empty; `S` on the Cheats page saves it) and `W` writes the
  typed value once. The first 500 results are listed; the count is always shown.

**Native pause menu.** `Esc` or `F1` (`[hotkeys] menu`; gamepad Start+Select) freezes the game
and opens the menu; Esc no longer quits directly (Quit is a menu item with confirmation). Items:
Resume; Save / Load state (slots 1-4 with thumbnails and timestamps, same slots as the F5/F7
hotkeys); Settings (initial resolution 1x/2x/4x/8x with dimensions, scale mode, filter, aspect,
volume, fullscreen — applied live and saved to `settings.ini`); Controls (keyboard + gamepad
bindings and all hotkeys, always accurate); Memory card (back up `card1.mcd` to a timestamped
copy, use any card file as the live card, switch between files); About (version, build, credits);
Quit. Keyboard: arrows / Enter / Esc back. Gamepad: d-pad / south / east. The window is resizable
and the picture adapts (Resolution sets the initial size); fullscreen stays borderless desktop.
Card restores refuse while the game holds card files open; best done on the title screen.

**Save states.** While playing, `F5` saves the game into the selected slot, `F7` loads it and `F6`
selects the next slot (1-4); a short notice confirms each ("State 2 saved", "Slot 3", "No state in
slot 1"). They work while paused too, and from the pause menu (which shows thumbnails). The keys
are `save_state`, `load_state` and `state_slot` under `[hotkeys]` in `settings.ini`. Limits:
- States live in memory for the current run only: they are gone when the game closes, and cannot
  be written to disk or moved to another machine (they contain host stack addresses; see
  "Persistent save states" below).
- Memory cards are not part of a state: loading an older state does not undo a save written to
  `card1.mcd` since. Avoid loading a state taken in the middle of a memory-card save.
- Supported by the Linux build (glibc, x86-64 / ARM64) and the Windows build made with MinGW (the
  release `.exe`). An MSVC build or macOS shows "save states are not supported on this platform".

For scripted checks, `DCB_STATE_SAVE_AT=<frame>[,...]` / `DCB_STATE_LOAD_AT=<frame>[,...]` save and
load the selected slot after that many frames, `DCB_EXIT_AT=<frame>` quits cleanly, and
`DCB_STATE_STRESS=<n>` saves, runs *n* frames, loads and runs them again, and aborts if the
machine differs (every frame boundary with *n=1*). Frame numbers count every frame run, as the
`DCB_SNAPSHOT` file names do: after a load at *M* of a state saved at *N*, snapshot *M+k* equals
snapshot *N+k* of a run without the load. With `DCB_RECORD`, loading a state rewinds the recording
to the loaded frame, so the log replays the timeline that was finally played.
`DCB_STATE_DUMP_AT=<frame>` + `DCB_STATE_DUMP_PATH=<file>` writes the slot bytes for offline
analysis (`tools/re/scan_stacks.py` classifies host pointers on game stacks).

**Persistent save states (verdict: not reasonably feasible).** A state saved to disk cannot be
loaded after a restart, and should not be attempted:
- The binary is PIE: code, statics, heap and stacks all land at different addresses every run
  (verified: three runs, three disjoint address sets).
- Game stacks at a frame boundary hold return addresses into our `.text`, pointers to long-lived
  heap objects (`Machine`, `Bios`, `Mmio`, `System`, the 2 MB guest RAM buffer), pointers to
  statics, and main-thread stack addresses (`tools/re/scan_stacks.py` on a real state: 61 code,
  45 heap, 42 binary-data, 33 main-stack values in 4 KB of stacks).
- Fixing the stacks (`MAP_FIXED_NOREPLACE`, verified working) is the easy 10%: the heap objects,
  statics and code addresses would all need fixing too (non-PIE build + fixed arenas), and then
  ASLR-disabled libc/SDL addresses inside `ucontext_t` and C++ exception state would still break.
- What works instead: memory-card backup/restore from the pause menu (robust cross-session save),
  plus a build-identity hash if states are ever written to disk (refuse foreign states clearly).
  See `docs/HOST_MAIN_LOOP.md` for the full analysis.

**Native movies.** The game's three movies (`movie0` opening, `movie1` credits, `movie2` BANDAI
logo) play natively when `movie/movie<N>.mpg` is in the asset pack or folder: full resolution,
their own audio, any key skips; without them the disc movie plays. With native movies the game
reads nothing through the CD drive. Files are MPEG-1 video + MP2 audio (decoded with
[pl_mpeg](third_party/pl_mpeg)); MPEG-1 has no 15 fps mode, so use 30:

```sh
# 1. Cut the disc movie into its three parts (config/<serial>/movies.json) at their true frame
#    rate (the opening streams at double speed: 30 fps; ffmpeg's reader assumes 15):
tools/disc/rip_movies.py        # -> assets/<serial>/movie_src/movie<N>.mp4 + converted .mpg
# 2. Your (upscaled) movie -> MPEG-1, then re-pack:
ffmpeg -i movie0_upscaled.mp4 -c:v mpeg1video -q:v 2 -r 30 -c:a mp2 -b:a 256k -ar 44100 -f mpeg \
       assets/converted/SLPS-03101/movie/movie0.mpg
./build/linux-debug/dcb_asset_ripper pack assets/converted/SLPS-03101 assets/SLPS-03101.pak
```

Save states are refused while a native movie plays. `DCB_TRACE_MOVIE=1` prints, per second,
host frames, movie audio samples and video frames (they should read ~60 / 44100 / the movie's fps).

**Native file access.** The game's file API (open/read/close over the `X.DRV` archives, and
libcd's `CdSearchFile`) is replaced by native code (`src/game/overrides/files.cpp`): data files
are read straight from the game data, instantly, instead of through the emulated CD drive. A
loose file at `assets/<serial>/files/<X>/<DIR>/<NAME.EXT>` (e.g. `assets/SLPS-03101/files/B/CARD2.CDD`)
replaces that file whatever its size. `DCB_LOG_FILES=1` logs every file the game opens;
`DCB_CD_FILES=1` restores the original CD path for comparison. At exit the game prints how many
sectors went through the CD drive: data should be 0, only the intro movie still streams.

**File overrides.** A file placed in `assets/<serial>/disc/<name>` (named as under `fs/`;
`extracted/<serial>/overrides/` also works but is deprecated) replaces that disc file when it has exactly the same size; other sizes are refused and
logged. Raw movie sectors get their headers re-stamped with this disc's positions, so a movie
from another pressing plays as if it were on this disc. Each active override is logged at start
(`[disc] override: ...`). Example, the English intro movie from the US disc (import the US dump
first, `dcb --import <us.cue> <dir>`):

```sh
mkdir -p assets/SLPS-03101/disc
cp <dir>/SLUS-01328/fs/DIGIMON.MOV.raw2352 assets/SLPS-03101/disc/
```

**English text.** A native override draws plain-ASCII strings with the US font and widths, while
Shift-JIS strings keep going through the JP renderer (untranslated Japanese still shows). The
English data is built from the player's own dumps into gitignored `assets/`:

```sh
python3 tools/text/en_text.py --jp extracted/SLPS-03101 --us extracted/SLUS-01328 --out assets/SLPS-03101
# -> en_font.bin (US font rows + width table), files/B/CARD2.CDD and DECK2.DEK (US card and deck
#    names, attacks, effect lines), en_text_report.txt (lines too long for the JP slots)
```

Without `en_font.bin` the game draws everything with the JP renderer; deleting `files/B/` brings
the Japanese card and deck text back. The font lives in a private texture sheet in the GPU, not
in the game's VRAM. Notes: [docs/re/text-engine.md](docs/re/text-engine.md).

**Community fixes.** Fixes published for this game on romhacking.net (made for the US disc image)
are applied to SLPS-03101 too. Download them yourself (they are their authors' work, never in this
repo) and put the `.xdelta` files in `assets/SLPS-03101/fixes/`; the English converter above then
applies them to the US reference data it takes English from (card text, strings, scripts), and
carries data changes in the overlays into the SLPS overlays (`files/P/`). Supported and tested:

| Fix | What it does here |
|---|---|
| [Digi-Parts Fix](https://www.romhacking.net/hacks/8474/) (hack 8474) | Digi-Part 037 (Eat-up HP) no longer missable with the Veemon, Gatomon or Wormmon partner: the SLPS overlays have the same table and the same bug |
| [Effect Text Fix v2](https://www.romhacking.net/hacks/9360/) (hack 9360) | effect text of Aquilamon, Dolphmon, AeroVeedramon, Sylphymon, Veedramon, Ankylomon and Special Digivolve matches what the cards do; Tentomon's attack reads Super Shocker |

The converter prints each fix it applied and every byte it carried over (`fix: ...`); remove a
file from `fixes/` and re-run it to drop that fix. Notes: [tools/text/fixes.py](tools/text/fixes.py).

**Game assets (textures).** `dcb_asset_ripper` (built with the tools) rips the images and sound
banks from the game data into `assets/` (gitignored), and packs them into one file:

```sh
./build/linux-debug/dcb_asset_ripper unpack extracted/SLPS-03101        # -> assets/raw/, assets/converted/SLPS-03101/
./build/linux-debug/dcb_asset_ripper sfx assets/raw/SLPS-03101 --game SLPS-03101   # sound banks + sfx_manifest.json
./build/linux-debug/dcb_asset_ripper pack assets/converted/SLPS-03101 assets/SLPS-03101.pak
```

At start the game loads replacement textures from the first of: `DCB_HD_PACK=<.pak|folder>`,
`assets/<serial>.pak`, `assets/converted/<serial>/`; the manifest (`assets_manifest.json`) is read
from inside the pack or folder unless `DCB_HD_MANIFEST=<file>` names one. Edit a PNG (same size as the
original for now: the renderer draws at native resolution), re-pack, restart. Palette images are
converted against their own palette from the disc (stored in the manifest), so an edit should use
that palette's colours; the game still chooses the palette when drawing, so palette animation keeps
working. Manifests ripped before this change lack the palettes: re-rip (this rewrites
`assets/converted/<serial>/`, so keep a copy of edited PNGs). Unmodified art gives
frames bit-identical to the original. PNG alpha: 0 = transparent, 255 = opaque; the semi-transparency
bit is taken from the original pixel unless alpha is exactly 254 (forces it on). At exit the game
prints how many texture uploads were replaced and why others were not; `DCB_LOG_HD=1` (or
`./dcb.sh -H`) logs each texture as it is replaced (file, size, format); `DCB_TRACE_HD=<n>` logs the
first *n* uploads that match no manifest entry (movie frames arrive as 24-pixel-wide strips and never match). `DCB_LOG_TEX=1`
(with `DCB_LOG_LOADS=1`: `./dcb.sh -T`) logs every distinct texture and palette upload once: frame,
VRAM position and size, the file it came from, and what was committed (original, a replacement
PNG, or US raw data), so a glitch on screen can be traced to its file and manifest entry.

**US images in the JP game.** With both games ripped (`assets/converted/SLPS-03101/` and
`assets/converted/SLUS-01328/`), `tools/assets/swap_us_images.py` makes the JP game show the US
images wherever the layout is the same: attack names, mini cards, battle UI, card art, the
opening, partner, friend and trade screens, the city menus and city-name plates, the card menu
and the fusion screens (about 980 images and 590 palettes). The US data goes
in exactly as the US disc has it: each changed image (and its palette, where the US build changed
it) is written to `assets/converted/SLPS-03101/us/*.raw`, and the manifest entry points there. A
manifest path ending in `.raw` is uploaded as it is, with no palette conversion, so colours and
palette animation match the US game; the JP PNGs are not touched. Images pair by their layout on
the disc, not by file name; the script lists what it leaves alone (the title, the MATCH/WIN name
plates, attacks with no US version) and, where one JP image stands for attacks the US build named
differently, which one it picked. Run it without options for a dry run, with `--apply` to write
(the manifest is backed up to `assets/SLPS-03101/backup/us_images/`), or `--restore` to undo;
then re-pack. `SYSTEM.TIM` is never swapped (the JP text engine draws its font from it), and the
city HELP MENU plate stays JP (the US one names the US buttons). `--root DIR` runs it on a copy
(`DIR/assets`, `DIR/extracted`).

**Resizing sprites.** The game draws each sprite one texel per pixel, at its original size. When
replacement art needs a different size on screen (a longer English line, a smaller logo), put a
`sprites.txt` next to the manifest (in `assets/converted/<serial>/`, then re-pack): one rule per
line, `tex_x tex_y u v w h draw_w draw_h [src_w src_h]`. Each rule draws the matching sprite at
`draw_w` x `draw_h`, centred where the game put it, showing `src_w` x `src_h` texels (default: the
sprite's own). Different sizes scale; equal sizes draw one texel per pixel.

To show wider art without scaling it (the US title's 256-wide copyright in the JP 176-wide slot),
give the image a bigger slot in video memory: add `"slot_w"` (and/or `"slot_h"`) in pixels to its
manifest entry, and the art is uploaded at that size from the same corner. Palette images only. The
area it grows into must be free on that screen; the US version of the same screen shows where
that's safe. Then add a rule with `src` = `draw` = the slot size. `DCB_TRACE_PRIMS=1` prints every distinct
textured draw once (packet address, draw mode, raw GP0 words), which is where the numbers come from:
`tex_x` = (mode & 15) × 64, `tex_y` = ((mode >> 4) & 1) × 256, and u, v, w, h come from the sprite's words.

**Reverse engineering.** `DCB_WATCH=800E0000-800E1800` (or `./dcb.sh -W 800E0000-800E1800`) logs
every write the game makes into those RAM ranges, with the old and new value, the function that
wrote it and its callers: do something in the game (win a card, level up) and read which address
changed and which code changed it. `DCB_COVERAGE=<file>` writes per-function call counts (address, overlay,
calls, first frame) at exit; `DCB_TRACE_CALLS=<n>` logs the first *n* calls live. `DCB_LOG_LOADS=1`
logs named asset loads (disc files, SPU uploads, XA streams, MDEC decodes) with frame numbers.
`tools/re/coverage_diff.py a.json b.json` diffs two coverage runs. Full loop (play → trace →
document → poke) in [`docs/RE_WORKFLOW.md`](docs/RE_WORKFLOW.md); subsystem notes in `docs/re/`.

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

## Credits

- **Digi-Parts Fix** ([romhacking.net hack 8474](https://www.romhacking.net/hacks/8474/)) by
  AUTHOR_8474: the missable Digi-Part 037 table fix, applied here to the SLPS overlays.
- **Effect Text Fix** ([romhacking.net hack 9360](https://www.romhacking.net/hacks/9360/)) by
  AUTHOR_9360: corrected card effect text, used as the English for those cards.
