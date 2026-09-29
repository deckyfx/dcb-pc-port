# Building

[Home](Home.md)

> Copyrighted inputs (`disc/`, `bios/`, `extracted/`, `assets/`, `generated/`) are gitignored. Never commit them.

## Repository layout

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
tools/disc/             extract_disc.py (+ tests), verify_import.sh, pdrv_segments.py,
                        rip_movies.py, drv_unpack.py, drv_diff.py
tools/ghidra/           setup_ghidra_mcp.sh, import_ghidra.sh, ghidra_gui.sh
tools/recomp/           the MIPS→C recompiler (C++ host tool)
tools/asset_ripper.cpp  dcb_asset_ripper: rip / pack textures and sound banks
tools/assets/           TIM / VAB / XA / STR converters, swap_us_images.py (+ tests)
tools/text/             en_text.py: English font and card/deck text from the US dump,
                        catalog.py (text catalog), fixes.py (community fixes) (+ tests)
tools/re/               coverage_diff.py, scan_stacks.py
tests/                  runtime unit tests (ctest)
```

The HLE layer is described in [src/hle/README.md](../../src/hle/README.md).

## Workflow

```bash
# 1. Disc → filesystem + boot EXE (dev tool: also writes exe/boot.* for Ghidra and a manifest;
#    players use the native importer instead, see Game-Data.md — same layout.txt/fs/iso_meta.bin)
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

The recompiler reads `extracted/<serial>/exe/boot.exe` (written only by `extract_disc.py`, not by
the native importer) and the committed `config/<serial>/` files, so a plain build from source needs
steps 1, 4 and 6. Step 3 regenerates `config/<serial>/overlays.json`; steps 2 and 5 import into and
synchronize Ghidra; step 4 also writes `generated/<serial>/discovered.json`; see
[Ghidra](Ghidra.md). Until the recompiler has run, the build links an empty function table. Running a
built `dcb` only needs the game data, [imported](Game-Data.md) once.

Select the target with `-DDCB_GAME_ID=SLUS-01328` (default: `SLPS-03101`).

CMake presets: `linux-debug`, `linux-release`, `windows-cross` (MinGW, from Linux) and
`windows-msvc`.

## dcb.sh

`./dcb.sh` builds (Debug) and runs in one step, keeping a copy of every run in `logs/run.log`. It
expects the disc image in `disc/<serial>/`. Flags combine (`./dcb.sh -r -t`):

| Flag | Effect |
|---|---|
| `-r` | regenerate C from MIPS first (after recompiler/config changes) |
| `-t` | trace every BIOS call (`DCB_TRACE_BIOS=1`) |
| `-g` | run under gdb; stops where the game hits something unimplemented |
| `-b` | build only, don't run |
| `-w` | also cross-build the Windows .exe (`build/windows-cross/dcb.exe`) |
| `-s SERIAL` | game id (default `SLPS-03101`) |
| `-H` | log each replacement texture as it is used (`DCB_LOG_HD=1`) |
| `-T` | log every file load and texture upload, with its source file (`DCB_LOG_LOADS=1 DCB_LOG_TEX=1`) |
| `-W RANGES` | log the game's writes to RAM ranges, e.g. `-W 800E0000-800E1800` (`DCB_WATCH`) |
| `-G` | open Ghidra (`ghidra/ghidra_12.1.2_PUBLIC`, MCP scripting on) instead |

## CI

Linux tests and a Windows `.exe` are built by CI (manual trigger for now).
