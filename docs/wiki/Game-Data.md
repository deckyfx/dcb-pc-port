# Game data

[Home](Home.md)

The program ships without any game data. The player imports a dump of their own disc once,
natively (no Python needed), and the disc image is no longer needed afterwards.

## Importing a disc

```bash
dcb --import <disc.cue|disc.bin> [dest-dir] [--force]   # default dest-dir: ./extracted
```

This reads a raw `.cue`/`.bin` dump (Mode 2, 2352-byte sectors; a `.bin` without its `.cue` works
too), identifies the game from `SYSTEM.CNF` and writes `extracted/<serial>/`: `layout.txt`,
`iso_meta.bin` and `fs/` (XA/STR files as whole sectors, `*.raw2352`). The output is byte-identical
to `tools/disc/extract_disc.py` (checked by `ctest` on a synthetic disc, and on a real dump with
`tools/disc/verify_import.sh <disc.cue>`).

The import goes to a temporary directory that is renamed into place only when complete, checks free
space first, and rejects wrong input with a clear message: cooked `.iso`/`.chd`/`.pbp`, an audio
track, a non-PlayStation disc, a truncated dump, or another game.

Accepted discs:

- **SLPS-03101**: the version this port plays.
- **SLUS-01328**: the source of the English text and art grafted onto SLPS-03101 (see
  [English Text](English-Text.md) and [Textures](Textures.md)).

Other serials are refused: this build could not run them, so importing would only fill the disk.

## First run

When no game data is found, the SDL build explains what is needed, opens the system file dialog
(`.cue`/`.bin`), imports with a progress window ("Importing… NN%", close it to cancel) and boots.
Picking the US disc imports it and asks again for the Japanese one. Headless runs
(`DCB_HEADLESS=1`) print these instructions and exit with status 1. After the import the disc image
is no longer needed. (`DCB_IMPORT_IMAGE=<path>` skips the explanation and the dialog, for automated
tests of the flow.)

## Where data is found

`dcb [extracted-dir | disc.cue | disc.bin]`. Without an argument it uses, in order: `DCB_DISC`,
`extracted/<serial>/` (imported data), then a `.cue`/`.bin` in `disc/<serial>/` or the current
directory.

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

Example, the English intro movie from the US disc (import the US dump first,
`dcb --import <us.cue> <dir>`):

```sh
mkdir -p assets/SLPS-03101/disc
cp <dir>/SLUS-01328/fs/DIGIMON.MOV.raw2352 assets/SLPS-03101/disc/
```
