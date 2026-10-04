# Textures and game assets

[Home](Home.md)

## Ripping and packing

`dcb_asset_ripper` (built with the tools) rips the images and sound banks from the game data into
`assets/` (gitignored), and packs them into one file:

```sh
./build/linux-debug/dcb_asset_ripper unpack extracted/SLPS-03101        # -> assets/raw/, assets/converted/SLPS-03101/
./build/linux-debug/dcb_asset_ripper sfx assets/raw/SLPS-03101 --game SLPS-03101   # sound banks + sfx_manifest.json
./build/linux-debug/dcb_asset_ripper pack assets/converted/SLPS-03101 assets/SLPS-03101.pak
```

Full usage (run it without arguments to print it):

```
unpack <extracted/serial|drv-dir|file.DRV> [-o out] [--game ID] [--lba-map manifest.json]
pack <asset-dir> <out.pak>
sfx [raw-dir|file.bin] [-o out] [--game ID]
```

## Replacement textures

At start the game loads replacement textures from the first of: `DCB_HD_PACK=<.pak|folder>`,
`assets/<serial>.pak`, `assets/converted/<serial>/`; the manifest (`assets_manifest.json`) is read
from inside the pack or folder unless `DCB_HD_MANIFEST=<file>` names one.

Edit a PNG (same size as the original for now: the renderer draws at native resolution), re-pack,
restart. Unmodified art gives frames bit-identical to the original.

- **Palettes.** Palette images are converted against their own palette from the disc (stored in the
  manifest), so an edit should use that palette's colours; the game still chooses the palette when
  drawing, so palette animation keeps working. Manifests ripped before this change lack the
  palettes: re-rip (this rewrites `assets/converted/<serial>/`, so keep a copy of edited PNGs).
- **Alpha.** PNG alpha: 0 = transparent, 255 = opaque; the semi-transparency bit is taken from the
  original pixel unless alpha is exactly 254 (forces it on).

Diagnostics:

- At exit the game prints how many texture uploads were replaced and why others were not.
- `DCB_LOG_HD=1` (or `./dcb.sh -H`) logs each texture as it is replaced (file, size, format).
- `DCB_TRACE_HD=<n>` logs the first *n* uploads that match no manifest entry (movie frames arrive
  as 24-pixel-wide strips and never match).
- `DCB_LOG_TEX=1` (with `DCB_LOG_LOADS=1`: `./dcb.sh -T`) logs every distinct texture and palette
  upload once: frame, VRAM position and size, the file it came from, and what was committed
  (original, a replacement PNG, or US raw data), so a glitch on screen can be traced to its file
  and manifest entry.

Higher-resolution art needs a renderer with a higher internal resolution; that is not done yet.

## US images in the JP game

With both games ripped (`assets/converted/SLPS-03101/` and `assets/converted/SLUS-01328/`),
`tools/assets/swap_us_images.py` makes the JP game show the US images wherever the layout is the
same: attack names, mini cards, battle UI, card art, the opening, partner, friend and trade
screens, the city menus and city-name plates, the card menu and the fusion screens, the VS and
result screens, the title (about 1115 images and 715 palettes).

The US data goes in exactly as the US disc has it: each changed image (and its palette, where the
US build changed it) is written to `assets/converted/SLPS-03101/us/*.raw`, and the manifest entry
points there. A manifest path ending in `.raw` is uploaded as it is, with no palette conversion, so
colours and palette animation match the US game; the JP PNGs are not touched.

Images pair by their layout on the disc, not by file name; the script lists what it leaves alone
(the MATCH/WIN name plates, attacks with no US version) and, where one JP image stands
for attacks the US build named differently, which one it picked. `SYSTEM.TIM` is never swapped (the
JP text engine draws its font from it). The opponent name pictures on the VS screen are left to
`tools/text/bigfont.py`, which grafts them into the MATCH archives.

A few images need more than a swap:

- **City HELP MENU plate** (also on the world map): the US plate says ✕ Enter / △ Menu, this build
  keeps the JP controls (○ enters, ✕ opens the menu). The tool builds it from both dumps: the US
  plate with its ✕ icon moved to the Menu row and the JP ○ icon on the Enter row (the colours the
  US palette lacks take the slots the dropped △ used).
- **VS / result screen record strip**: the JP picture has the kanji 戦 勝 敗 in it, the US one is
  blank and wider. The tool cuts the US strip to the JP width, and the game draws "Btl.", "W" and
  "L" where the kanji were (only when the strip in video memory is the blank one).
- **1st / 2nd turn cards, portraits, WIN / LOSS banners** (MATCH and WIN archives): plain swaps.
- **Battle phase banner** (Prep / Digi-volve / Battle, `B:\CBTL_SYS.ARC`): the US file uploads its
  palette as one 32-colour row, the JP one as two 16-colour rows (the game draws the banner with
  the first row and its fading trail with the second). The colours are the same, so the tool
  sends the US palette up in the JP shape and swaps the image.
- **Title** (`B:\TITLE.ARC`, a different image list): the US subtitle (DIGITAL CARD BATTLE, 320x48),
  copyright (256 wide) and NEW GAME / CONTINUE / Battle with Friend go into the JP images the
  way the texture replacer fits a PNG (box-downsampled to the JP size, quantized to the JP
  palette, which the logo and the D-1 Grand Prix label share). The copyright gets a 256-wide slot
  (`"slot_w"`); `config/SLPS-03101/sprites.txt` draws it 1:1 from there and the subtitle smaller.
  The JP logo stays. Hand-edited art in `assets/<serial>/custom/textures/` wins over this (and
  over any US image); `pack.sh -r` puts `custom/sprites.txt` over ours when there is one.

| Option | Effect |
|---|---|
| (none) | dry run |
| `--apply` | write (the manifest is backed up to `assets/SLPS-03101/backup/us_images/`) |
| `--restore` | undo: the JP manifest as it was, no US data |
| `--only REGEX` | limit the run by a regex on `DRV:entry path` (e.g. `B.DRV:M_CARD`) |
| `--root DIR` | run on a copy (`DIR/assets`, `DIR/extracted`) |

Then re-pack.

## Resizing sprites

The game draws each sprite one texel per pixel, at its original size. When replacement art needs a
different size on screen (a longer English line, a smaller logo), put a `sprites.txt` next to the
manifest (in `assets/converted/<serial>/`, then re-pack): one rule per line,
`tex_x tex_y u v w h draw_w draw_h [src_w src_h]`. Each rule draws the matching sprite at
`draw_w` x `draw_h`, centred where the game put it, showing `src_w` x `src_h` texels (default: the
sprite's own). Different sizes scale; equal sizes draw one texel per pixel.

To show wider art without scaling it (the US title's 256-wide copyright in the JP 176-wide slot),
give the image a bigger slot in video memory: add `"slot_w"` (and/or `"slot_h"`) in pixels to its
manifest entry, and the art is uploaded at that size from the same corner. Palette images only. The
area it grows into must be free on that screen; the US version of the same screen shows where
that's safe. Then add a rule with `src` = `draw` = the slot size.

`DCB_TRACE_PRIMS=1` prints every distinct textured draw once (packet address, draw mode, raw GP0
words), which is where the numbers come from: `tex_x` = (mode & 15) × 64,
`tex_y` = ((mode >> 4) & 1) × 256, and u, v, w, h come from the sprite's words.
