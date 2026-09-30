# Game data

[Home](Home.md)

The program ships without any game data. The player provides dumps of their own two discs once,
natively (no Python needed), and the disc images are no longer needed afterwards:

- **SLPS-03101** (Digimon World: Digital Card Arena, Japan): the game that runs.
- **SLUS-01328** (Digimon Digital Card Battle, North America): the source of the English text and
  art grafted onto SLPS-03101 (see [English Text](English-Text.md) and [Textures](Textures.md)).

## Setup

```bash
dcb --setup <jp.cue|jp.bin> <us.cue|us.bin> [--fixes DIR] [--no-verify] [--force]
```

The whole first run from a terminal, in the current directory (the discs may be given in either
order):

1. **Verify** each image against its [redump.org](http://redump.org) entry: the data track's size
   first (a wrong dump fails at once), then its SHA-1 (reads the whole `.bin`, about 280 MB). A
   mismatch is refused with the expected size / hash; a damaged, patched or translated image, or
   another pressing, cannot be used.

   | Disc | Data track size | SHA-1 | redump |
   |---|---|---|---|
   | SLPS-03101 | 278996592 | `6ebf547972205b8cdd07d1014b0835bda0ce2b70` | [1685](http://redump.org/disc/1685/) |
   | SLUS-01328 | 215661936 | `b3945b3e76c1fcc554a7614e2b4211d974990105` | [636](http://redump.org/disc/636/) |

   `--no-verify` (or `DCB_NO_VERIFY=1`, also for the first-run window) skips this, for developers
   working with modified images.
2. **Import** both into `assets/dump/<serial>/` (see below; an existing complete import is kept).
3. **Build the English data** from the two imports (`src/patch`, the C++ port of the Python
   pipeline) into `assets/SLPS-03101/` and `assets/SLPS-03101.pak`. `--fixes DIR` applies the
   community fix files (`.xdelta`) in DIR; they are optional, made by other players, and never
   shipped with the program. `--force` builds even over English data this setup did not make (by
   default such data, e.g. a bundle's, is left alone).

The build writes `assets/SLPS-03101/english.stamp` last; until then the stamp says the data is
unfinished, so an interrupted or failed build is simply run again.

## Importing a disc

```bash
dcb --import <disc.cue|disc.bin> [dest-dir] [--force]   # default dest-dir: ./assets/dump
```

The import alone (no verification, no English data), for either disc. This reads a raw
`.cue`/`.bin` dump (Mode 2, 2352-byte sectors; a `.bin` without its `.cue` works too), identifies
the game from `SYSTEM.CNF` and writes `assets/dump/<serial>/`: `layout.txt`, `iso_meta.bin` and
`fs/` (XA/STR files as whole sectors, `*.raw2352`). The output is byte-identical to
`tools/disc/extract_disc.py` (checked by `ctest` on a synthetic disc, and on a real dump with
`tools/disc/verify_import.sh <disc.cue>`).

The import goes to a temporary directory that is renamed into place only when complete, checks free
space first, and rejects wrong input with a clear message: cooked `.iso`/`.chd`/`.pbp`, an audio
track, a non-PlayStation disc, a truncated dump, or another game. Only the two discs above are
accepted: this build could not run anything else, so importing would only fill the disk.

## First run

The SDL build runs the setup in a window when it is needed:

- the Japanese game data is missing, or
- it is there, but there is no English data at all (neither `assets/SLPS-03101.pak` nor
  `assets/SLPS-03101/text/`, next to the executable or in the current directory), or
- `english.stamp` exists but is unfinished or from an older builder version.

The private / developer bundles carry English data made by the Python pipeline and no stamp, so
they never trigger it (and are never overwritten).

The window explains what is needed, then asks for the Japanese disc and the North American one in
turn (system file dialog, `.cue`/`.bin`); each is verified and imported with a progress window
("Checking… NN%"; close the window to cancel). Picking the other disc first imports it too and asks
again. Then it offers the optional fixes folder (Skip / Choose folder...), builds the English data
with a progress window, and boots. When the build fails the error is shown and the game is not
started; quitting at any point leaves everything to be continued on the next start. Only the
missing steps are shown (e.g. just the Japanese disc when the English data is already there).

Headless runs (`DCB_HEADLESS=1`, or no display) without game data print the instructions and exit
with status 1; with the game data but no English data they print a note and run the game in
Japanese.

For automated tests of the window flow, `DCB_IMPORT_IMAGE=<jp image>`,
`DCB_IMPORT_US_IMAGE=<us image>` and `DCB_SETUP_FIXES=<dir>` replace the explanations and dialogs
(one attempt each).

## Where data is found

`dcb [--no-verify] [data-dir | disc.cue | disc.bin]`. Without an argument it uses, in order:
`assets/dump/<serial>/` next to the executable, `DCB_DISC`, `assets/dump/<serial>/` (imported data)
and the older `extracted/<serial>/` in the current directory, then a `.cue`/`.bin` in
`disc/<serial>/` or the current directory.

Extracted data rebuilds every CD sector on demand, so the game runs from it alone (frame-for-frame
identical to the disc image), and individual files can later be replaced (translation, enhanced
assets).

## Native file access

The game's file API (open/read/close over the `X.DRV` archives, and libcd's `CdSearchFile`) is
replaced by native code (`src/game/overrides/files.cpp`): data files are read straight from the
game data, instantly, instead of through the emulated CD drive.

A loose file at `assets/<serial>/files/<X>/<DIR>/<NAME.EXT>` (e.g.
`assets/SLPS-03101/files/B/CARD2.CDD`) replaces that file whatever its size.

- `DCB_LOG_FILES=1` logs every file the game opens.
- `DCB_CD_FILES=1` restores the original CD path for comparison.

At exit the game prints how many sectors went through the CD drive: data should be 0, only the
intro movie still streams (none at all with [native movies](Movies.md)).

## Disc file overrides

A file placed in `assets/<serial>/disc/<name>` (named as under `fs/`; `extracted/<serial>/overrides/`
also works but is deprecated) replaces that disc file when it has exactly the same size; other sizes
are refused and logged. Raw movie sectors get their headers re-stamped with this disc's positions,
so a movie from another pressing plays as if it were on this disc. Each active override is logged
at start (`[disc] override: ...`).

Example, the English intro movie from the US disc (the setup does this itself; by hand, import the
US dump first, `dcb --import <us.cue> <dir>`):

```sh
mkdir -p assets/SLPS-03101/disc
cp <dir>/SLUS-01328/fs/DIGIMON.MOV.raw2352 assets/SLPS-03101/disc/
```
