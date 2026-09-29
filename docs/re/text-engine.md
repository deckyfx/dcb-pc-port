# Text engine (SLPS-03101, with SLUS-01328 for comparison)

Reverse-engineering notes on how the JP game stores, lays out and draws text, as groundwork for an
English build that keeps the JP code and uses the US text and fonts
([HYBRID_EN_ASSETS.md §4](../HYBRID_EN_ASSETS.md#4-text-q3) has the asset-level picture).

Evidence sources: Ghidra project `DCB` (JP program `/SLPS-03101/SLPS_031.01`, read only), the
recompiled C in `generated/SLPS-03101/*.c` (every `jal` is kept as a comment, so it gives the
complete static caller list, overlays included), and the raw game files. US code was read by
disassembling `SLUS_013.28` directly (the US program was not opened in the shared Ghidra
session). Confidence: **H** high (read in code), **M** medium (read in part, or inferred from
data), **L** low (a guess).

"Glyph", "cell" and pixel sizes refer to the 4-bpp textures on the PSX (1 VRAM halfword = 4 px).

## Contents

1. [Summary](#1-summary)
2. [Encoding and control codes](#2-encoding-and-control-codes)
3. [Rendering pipeline](#3-rendering-pipeline)
4. [Fonts](#4-fonts)
5. [Where strings come from](#5-where-strings-come-from)
6. [Text windows and layout](#6-text-windows-and-layout)
7. [Override plan](#7-override-plan)
8. [Proposed names](#8-proposed-names)
9. [Open questions](#9-open-questions)

## 1. Summary

* JP text is **Shift-JIS** with **inline ASCII control codes and no escape character**: `e3` in
  a string is a level icon, `c5` a colour switch, `\`+`n` a newline. A byte 0x81–0x98 starts a
  2-byte SJIS character. Other ASCII letters are either control codes or are **silently
  skipped**. ASCII digits are drawn as full-width digits. (H)
* US text is ASCII, with the same codes behind an escape character: `*e3`, `*c5`. (H)
* The main renderer is `FUN_8002ae00(x, y, clut, prop, rgb*, ot, str)`. Most callers use the
  wrapper `FUN_8002adc8(x, y, clut, prop, ot, str)`, which passes a grey 0x808080 colour. The
  kanji glyphs (12×11, 1 bpp, in `B:\KANJI0.FNT`/`KANJI1.FNT`) are expanded on demand into a
  VRAM glyph cache at (960, 0) and drawn as one SPRT per character. (H)
* The US renderer draws from a **fixed ASCII sheet inside `SYSTEM.TIM`** with a per-character
  width table. It has no cache and no kanji path. (H)
* The game has **five more text-like renderers** (condensed kanji, a 6×11 digit font, an 8×7
  half-width kana mini font, a 6×6 mini font, 16×21 big digits) and a **big-name font**
  (32×32 TIMs per character). (H)
* Several callers **expand variables before drawing** by scanning for bare ASCII letters: `P0`/`P1`
  (battle banner), `p` (tutorial), `hN` (Fusion Shop), `S`/`E` (memory-card slot, US `*S`/`*E`). With English text
  these scanners corrupt ordinary words ("Preparation" contains `P`). They need overrides too. (H
  for `P` and `p`, M for `S`/`E`)
* No renderer wraps text. Every line break is in the data. Windows size themselves from the
  measure function. (H)

## 2. Encoding and control codes

### 2.1 Byte classes (JP `FUN_8002ae00` / `FUN_8002b638`)

| Byte(s) | Meaning | Evidence | Conf. |
|---|---|---|---|
| `00` | end of string | loop head, `if (*s == 0)` | H |
| `81`–`98` + trail byte | 2-byte SJIS character → glyph cache → SPRT | `(byte)(*s + 0x7f) <= 0x17` then `FUN_8002aa30`/`FUN_8002ab64` | H |
| `81 40` (full-width space) | advance only (12 px, or the fixed cell width), nothing drawn | `FUN_8002aa30` returns 1 for `0x8140` | H |
| `0A` | newline: x = start, y += 13 + `h` | `case 10` | H |
| `5C 6E` (`\` `n`, 2 chars) | newline, same as `0A`. MSD scripts use this form. | `case 0x5c` | H |
| `20` | space: 6 px, or 12 px after `z` (condensed renderer 4/8) | `case 0x20`, `local_2c` | H |
| `30`–`39` | full-width digit ０–９ via the table at `0x80070a68` (10 SJIS codes, `82 4F`…`82 58`) | `default:` branch, `-0x7ff8f598` = `0x80070a68` | H |
| any other byte | skipped (1 byte, no advance) | `default:` falls through to `s = pbVar4 + 1` | H |

Lead bytes 0x99–0x9F and 0xE0–0xEF (JIS level-2 kanji) are **not** recognised as SJIS: level-2
kanji cannot be drawn. (H, from the range check)

### 2.2 Two-byte control codes

The JP engine has no escape character. The US engine uses the same letters behind `*`
(`*a`…`*w`). The US jump table is at `0x800102c0`, indexed by `c - 'a'` (23 entries: a b c d e g h
s w handled, the rest fall back to "draw the letter").

| JP code | US code | Meaning | Advance | Evidence (JP) | Conf. |
|---|---|---|---|---|---|
| `aN` | `*aN` | inline icon `N-'0'` (0–9) from `SYSTEM.TIM` | 12 px | `case 0x61` → `FUN_80029f70(x, y+1, 0, N-0x30, rgb, ot)` | H |
| `bN` | `*bN` | inline icon `N-0x29` (7–16) | 12 | `case 0x62` | H |
| `cN` | `*cN` | select text CLUT `N` (0–9). CLUT = (CLUTX + (N%2)·16, CLUTY + N/2). | 0 | `case 99`: `param_3 = N - 0x30` | H |
| `dN` | `*dN` | inline icon `N-0x1c` (20–29) | 12 | `case 100` | H |
| `eN` | `*eN` | inline icon: `N ≤ '3'` → `N-0x23`, `'a'` → 0x11, else `N-0x22`. Used as level badges (`e3` = level III). | 12 | `case 0x65` | H |
| `gN` | `*gN` | big icon (24×12) `N-'0'` | 25 | `case 0x67` → `FUN_80029f70(.., 2, ..)` | H |
| `hN` / `h-N` | `*hN` | extra line spacing (±N px) for later newlines | 0 | `case 0x68` | H |
| `sN` | `*sN` | proportional mode off (`s0`) or on (`s1`). Overrides the `prop` argument. | 0 | `case 0x73`: `param_4 = N - 0x30` | H |
| `wN` / `w-N` | `*wN` | letter spacing ±N px added after every glyph | 0 | `case 0x77` | H |
| `z` (1 byte) | – | toggle space width 6 ↔ 12 | 0 | `case 0x7a` | H |

The port adds one code of its own to the English renderer (neither disc has it; the US table stops
at `w`): `*yN` / `*y-N` draws the rest of the string N px lower / higher without moving the line
(measure and `g_text_h` unchanged); `src/platform/text_codes.hpp`, used for the unit labels of §7.9.

Codes that are **not** handled by the renderer but are expanded by the caller before drawing:

| Code (JP) | Code (US) | Expanded by | Replaced with | Conf. |
|---|---|---|---|---|
| `P0`, `P1` | `*P0`, `*P1` | battle banner `FUN_800466d0` | player name (player struct `+0x1ca`); index XOR the current side `DAT_801dafca` | H |
| `p` | `*p` | tutorial box `FUN_KAWSEG__801ed334` | own player name (`DAT_801daf40 + 0x1ca`) | H (the US `BETA.MSD` uses `*p`, §7.12) |
| `h0`–`h3` | `*h1` (the only one seen) | Fusion Shop line builder EVOSEG `801ebf4c` | `h0` player name (game_data + 0), `h1` the fusion result's card name, `h2`/`h3` the same with all but the first character as ？ | H |
| `S`, `E` | `*S`, `*E` | memory-card screens (OPENSEG) | slot number | M (seen in strings, expander not traced) |

### 2.3 Other encodings in the JP EXE

| Where | Encoding | Conf. |
|---|---|---|
| `FUN_800288c8` mini font (8×7) | single bytes: ASCII 0x20–0x7F, JIS X 0201 half-width katakana 0xA0–0xDD, dakuten 0xDE/0xDF (overlaid at −2,−2 without advance). `~` toggles katakana→hiragana rows. Codes: `01 N` mini icon, `0C N` CLUT, `0A` newline (+9). | H |
| `FUN_8002a37c` | converts an SJIS string to the mini-font encoding above (kana → half-width through `0x800709e2`/`0x80070a40`/`0x80070ad8`, `a`–`e` codes → `01 N`). **Other ASCII bytes are dropped.** | H |
| `FUN_80028cb4` 6×11 font | ASCII glyph `c - 0x20` at u = (c−0x20)·6, v = 0x8B. Codes `cN`, `wN`. Used for `sprintf("%4d")` numbers. | H |
| `FUN_80028554` 6×6 font | ASCII/half-width kana, v = 0x9E + row·6. Codes `0C N`, `0A`. | H |
| `FUN_80029010` big digits | `0-9 + - =` as 16×21 cells, u = 0x80 + (i%8)·16, v = (i/8)·21. Code `cN`. | H |

## 3. Rendering pipeline

### 3.1 Call graph

```
caller (menu / dialog / battle HUD / overlay)
 ├─ FUN_8002adc8(x,y,clut,prop,ot,str)          wrapper, rgb = 0x80070a64 (80 80 80)
 │   └─ FUN_8002ae00(x,y,clut,prop,rgb*,ot,str)  DRAW
 │       ├─ FUN_8002aa30(sjis*, out)            cache lookup (normal pages)
 │       ├─ FUN_8002ab64(sjis*, out)            cache insert (flushes all pages when full, retries)
 │       │   └─ FUN_800298d0(sjis*, RECT*)      expand 1-bpp glyph → 4-bpp + drop shadow, LoadImage
 │       │       └─ FUN_8002d130(sjis*)         glyph address in KANJI0/KANJI1
 │       │           └─ FUN_8002d1c0(sjis)      SJIS → glyph index
 │       ├─ FUN_80029ef4()                      primitive pool full? (-1 = stop drawing)
 │       └─ FUN_80029f70(x,y,mode,idx,rgb*,ot)  inline icon from SYSTEM.TIM (wrapper FUN_80029f40)
 ├─ FUN_8002b638(prop,str)                      MEASURE (same parser, no drawing)
 ├─ FUN_8002bd04 → FUN_8002bd3c                 condensed DRAW (8 px cells)
 │       ├─ FUN_8002b9bc / FUN_8002baec          condensed cache lookup / insert
 │       └─ FUN_8002959c                        condensed glyph expand
 └─ FUN_8002c574(prop,str)                      condensed MEASURE
```

Evidence: decompilation of each function (H), and `jal` comments in `generated/SLPS-03101/*.c` for
callers (H).

### 3.2 Core functions

| Address | Signature (proposed) | Returns | Notes | Conf. |
|---|---|---|---|---|
| `8002ae00` | `int text_draw(int x, int y, int clut, int prop, const u8 rgb[3], int ot, const char *s)` | max line width in px (also `g_text_w`). `g_text_h` = **absolute** y of the last line + 12. | One DR_TPAGE + SPRT per glyph (7 words, 0x1C bytes) from the pool at `g_text_prim` (`DAT_801d9714`), linked into OT entry `ot` of the current frame (`DAT_8007bf80 + 0x70 + ot*4`). Semi-transparent bit set. Glyph sprite 12 high, width = ink right edge (prop) or 12. Screen x = cursor − ink left edge. Returns early when the pool is full. | H |
| `8002adc8` | `int text_draw_grey(int x, int y, int clut, int prop, int ot, const char *s)` | as above | 85 static callers, most of them in overlays | H |
| `8002b638` | `int text_measure(int prop, const char *s)` | width; `g_text_h` = relative height (lines·13 − 1 + h-spacing) | Parses the same codes. `c` is skipped. Inserts glyphs into the cache as a side effect (it needs the ink extents). | H |
| `8002bd3c` / `8002bd04` | same as draw / draw_grey | | condensed: fixed cell 8, space 4/8, own cache pages (kind 1) | H |
| `8002c574` | `int text_measure_condensed(int prop, const char *s)` | | used by the dialog when bit 0x80 of its style is set | M |
| `8002aa30` | `int glyph_cache_find(const u8 *sjis, GlyphRef *out)` | 0 found, 1 blank (`81 40`), −1 miss | searches the 32 cache pages of kind 0 | H |
| `8002ab64` | `int glyph_cache_add(const u8 *sjis, GlyphRef *out)` | 0 / 1 | first page with `count < cap`. When all are full, it resets every count to 0 and calls itself (full flush). | H |
| `800298d0` | `void glyph_render(const u8 *sjis, RECT *dst)` | | 11 rows × 12 bits → 12×12 4-bpp: ink = index 1, shadow index 6 below and right, index 7 diagonal. Computes the ink extents `g_glyph_l`/`g_glyph_r` (`DAT_801d9702/03`): r += 3, minimum width 8 (centred), clamp 0..12. Uploads 3×12 halfwords through a staging ring (`DAT_801d96f4`, `DAT_801d96f2` entries of 0x48 bytes). | H |
| `8002d130` | `const u8 *kanji_glyph_addr(const u8 *sjis)` | `font + index*22` | SJIS 0x8140–0x84BE, or anything outside 0x889F–0x9872 → KANJI0 (`DAT_801d9718`), else KANJI1 (`DAT_801d971c`) | H |
| `8002d1c0` | `int kanji_glyph_index(u16 sjis)` | index | rows 0x81–0x84: 19 ranges from `{first_code, base_index}` pairs at `0x80070b0c`. Kanji: per half-row `(lead-0x88)*2 - (trail<0x7f)` into the pairs at `0x80070b58`. | H |
| `80029f70` | `void text_icon(int x, int y, int mode, int idx, const u8 rgb[3], int ot)` | | mode 0: 12×11 icon at u = (idx%14)·12, v = 0x69 + (idx/14)·11, CLUT row by range (0x15–0x17 → +5 and x+16, 0x1b → +3, 0x1c–0x24 → +7, else +5). Mode 1: 7×7 mini icon, v = 0x62. Mode 2: 24×12, u = 0xD8 (idx<10) or 0xC0. | H |
| `80029ef4` | `int text_prim_full(void)` | −1 when `g_text_prim == pool_end` | pool = `DAT_801d96f0` (1000) × 0x1C per frame buffer | H |

`GlyphRef` (8 bytes, filled by the cache functions): `+0 s16 vram_x` (halfwords), `+2 s16 vram_y`,
`+4 u8 ink_left`, `+5 u8 ink_right`, `+6 u16 tpage`. The draw function uses `u = (vram_x·4) & 0xFF`,
`v = vram_y & 0xFF`. (H)

### 3.3 State

| Global | Meaning | Set by | Conf. |
|---|---|---|---|
| `DAT_801d9704/06` | `SYSTEM.TIM` VRAM x,y (960, 256), used for the tpage of icons and small fonts | `FUN_800270d4` | H |
| `DAT_801d9710/12` | text CLUT block x,y = (992, 504): 16 CLUTs of 16 colours, CLUT `n` at (992 + (n%2)·16, 504 + n/2), uploaded from `DAT_801d7438` (32×8) | `FUN_800270d4` | H |
| `DAT_801d9714` | next free text primitive | `FUN_800270d4`, per frame `FUN_8002745c` | H |
| `DAT_801d96f0` | primitive pool size (1000) | `FUN_800270d4` | H |
| `DAT_801d96f2`, `DAT_801d96f4`, `DAT_801d96f8` | glyph staging ring: size (64), buffer, index | `FUN_800270d4` | H |
| `DAT_801d96fc` | 32 cache page descriptors × 12 bytes: `+0 u16 cap`, `+2 u16 count`, `+4 u16 tpage`, `+6 u8 kind` (0 normal, 1 condensed), `+8 GlyphSlot*` (`u16 sjis, u8 l, u8 r`) | `FUN_8002a7e4` (alloc), `FUN_8002a810` (clear), `FUN_8002a860(page, cap, kind)` | H |
| `DAT_801d9708 / 970c` | result width / height of the last draw or measure | draw, measure | H |
| `DAT_801d9718 / 971c` | KANJI0 / KANJI1 glyph data (after the 8-byte header) | `FUN_8002d564` | H |

Init: `FUN_80015728` (boot) → `FUN_800270d4(0x3c0, 0x100, 1000, 0x40)` → loads `SYSTEM.TIM`, the
fonts and the pools. Then `FUN_8002a860(15, 441, 0)` makes page 15 (VRAM x = 960 halfwords, y = 0,
21×21 cells of 12×12) the normal glyph cache. Overlays add a condensed page: OPENSEG and SUBSEG
use page 29, EVOSEG and SAISEG page 30, KAWSEG page 14. Each has 672 cells (32×21 of 8×12). (H)

The text CLUT palettes live in `DAT_801d7438` (BSS). The code that fills it was not traced. (L)

### 3.4 Callers by screen

The counts are static `jal` sites from the recompiled code.

| Screen | Function(s) | Renderer | String source | Conf. |
|---|---|---|---|---|
| Generic dialog / Yes-No box (33 callers in all overlays) | `FUN_80019ff0(win, text, style)` sets it up, `FUN_8001a590` draws it (callback, not a Ghidra function), `FUN_8001a284` runs it | draw_grey or condensed (style & 0x80), CLUT 7, prop 1 | caller string. Choice labels default to `0x80010028` "はい" / `0x80010030` "いいえ" | H |
| Battle card-info panel | `FUN_8003c0b0(panel, ot)` | draw (name, attacks, 4 effect lines), 6×11 digits, icons | card record in `CARD2.CDD` (+0x03 name, +0x26/+0x42/+0x5E attacks, +0xE7 effect lines) | H |
| Battle banner (top line) | `FUN_800466d0(brightness)` | draw at (16, 14 + slide), clip 288×12 | pointer table `0x80070e78` (18 entries), with `P0/P1` expansion | H |
| Battle help line (bottom) | `FUN_80046a3c(brightness)` | draw at (80, 0xDB − slide) | pointer table `0x80070ed4`, special `0x80011a2c` | H |
| Tutorial message | `FUN_KAWSEG__801ed334(y, str)` → callback `FUN_KAWSEG__801ed2d8` | measure + draw_grey, CLUT 7 | `B:\BETA.MSD` via the MSD VM, `p` expansion into a **144-byte stack buffer** (§7.12) | H |
| Fusion Shop (Andromon) lines | EVOSEG `801ebf4c` builds a line of the 4-line page, `801ec33c` reveals it, `801ec468` draws the page | draw_grey, CLUT 7 | `C:\EVENT\unit0%d.MSD` via the MSD VM (§7.12) | H |
| Pause/options dialogs in battle | `FUN_KAWSEG__801fa768` | dialog `FUN_80019ff0` | KAWSEG `.rodata` (`0x801e2524`…) | H |
| Big name (VS screen) | `bigname_load` `80044684(name, row, parent)` task, spawned by `vs_names_load` (KAWSEG `801F04FC`) | per-char TIM → VRAM | player name (both names in a battle with a friend); the opponent's name is a picture in `B:\MATCH\NNN.ARC` | H (§7.11) |
| System error screens | `FUN_8004c320`, `FUN_8004c410`, `FUN_8004c828` (table-dispatched) | measure, draw, 6×6 font | EXE `0x80012e90`… ("ＳＹＳＴＥＭ ＥＲＲＯＲ") | M |
| Overlay menus (OPENSEG, SUBSEG, SAISEG, EVOSEG, ENDSEG) | 20 direct `FUN_8002ae00` sites, 80 `FUN_8002adc8` sites, 12 `FUN_8002b638` sites | draw / measure | overlay `.rodata`, often via pointer tables | H (counts) |

## 4. Fonts

### 4.1 JP

| Font | Source | Format | VRAM | Index | Conf. |
|---|---|---|---|---|---|
| Kanji / kana (main) | `B:\KANJI0.FNT` (11 536 B), `B:\KANJI1.FNT` (65 240 B), loaded to RAM by `FUN_8002d564` | header `"TNF@"`, `u8 11` (rows), `u8 0x10`/`0x30`, `u16 count` (KANJI0: 524 = rows 0x81–0x84, KANJI1: 2965 = JIS level 1). Glyph = 11 × `u16` **big-endian**, bits 15..4 = 12 px. | cached on demand at (960, 0), 12×12 cells | `FUN_8002d1c0` | H |
| Condensed | same FNT files, `FUN_8002959c` squeezes to 8 px | | cache pages 14/29/30 | | M |
| Icons, mini fonts, digits | `B:\SYSTEM.TIM` (4 bpp, 256×256 at VRAM 960,256, CLUT at 960,496) | | icons v 0x69+, 6×11 digits v 0x8B, 6×6 v 0x9E, mini 8×7 v 0–~104 (kana rows at 42–104) | see §2.3 | H |
| Big name | `B:\FONT\%4.4X.tim` (232 TIMs named by SJIS code, 32×32 4 bpp), `FUN_80044684` | 1 TIM per character, loaded per name | x = 704 + i·8 halfwords (max 8 chars), y = 448 + row·32, CLUT (752, 471 + row) | sprintf of the SJIS code | H |

### 4.2 US

| Font | Source | Layout | Conf. |
|---|---|---|---|
| Main ASCII font | inside `B:\SYSTEM.TIM` (same TIM geometry as JP) | glyph `i = c − 0x20`: u = (i%16)·6 + (width[c] >> 4), v = 0x30 + (i/16)·12, sprite w = width[c] & 0xF (prop) or 6, h = 12 | H (disassembly of US `0x80028d48`) |
| Width table | US EXE `0x8006df9c` indexed by `c − 0x20` (= `0x8006df7c` indexed by `c`); byte = `uoff << 4 | advance`. For example `' '` = `04` (advance 4), `'!'` = `13` (u+1, advance 3). | | H |
| Icons | `SYSTEM.TIM`, row base v = 0x7F (JP 0x69) | | H (per HYBRID §4.3) |
| Big name | `B:\FONT.ARC` (20 848 B): 92 u32 offsets for `0x20`–`0x7B`, then 64 TIMs of 16×32 4 bpp (320 B each, one shared CLUT). Own glyphs: space, `-`, digits, A–Z, a–z; other codes point at the next glyph (`.` → `0`, `:` → `A`), `{` at the end of the file. Max 12 chars (US `FUN_80041ca8`) | | H (§7.11) |

The US ASCII sheet overwrites the rows that JP uses for the mini-font kana (v ≈ 42–115). **Loading the
US `SYSTEM.TIM` breaks the JP mini font's kana, and moves the icons.** (M)

Cell rows: the cells start at **sheet row 48** (the `+0x30` in the draw formula), so glyph `i` is at
sheet rows `48 + (i/16)·12 .. +11`; sheet rows 42–47 are the tail of the block above, and slicing
from row 42 instead splits every glyph across the row boundary (bottom half of one cell on top of
the top half of the next). The converter (`tools/text/en_text.py`) takes sheet rows 48–223 (the
extra rows are unused by the ASCII path). (H: verified by ASCII-art decode of the extracted rows)

### 4.3 US engine for reference

| US address | Signature | JP counterpart |
|---|---|---|
| `80028d48` | `int draw(int x, int y, const char *s, const u8 rgb[3], int clut, int ot)`, prop defaults to 1 | `8002ae00` (argument order differs) |
| `80028d18` | `draw_grey(x, y, s, clut, ot)` → rgb `0x8006df98` | `8002adc8` |
| `800293fc` | `int measure(const char *s)` (no prop argument, prop defaults to 1) | `8002b638` |
| globals | width/height `0x801d6b18/1c`, prim `0x801d6b24`, CLUT base `0x801d6b20/22`, SYSTEM.TIM x/y `0x801d6b12/14` | `0x801d9708/0c`, `9714`, `9710/12`, `9704/06` |

## 5. Where strings come from

### 5.1 Sources and formats

| Source | Format | How code finds a string | Conf. |
|---|---|---|---|
| Boot EXE `.rdata` | NUL-terminated, 4-byte aligned | direct `lui/addiu` or pointer tables (for example `0x80070e78` battle banner, `0x80070ed4` help line) | H |
| Overlays (P.DRV, load address `0x801E0B30`) | same | direct, or pointer tables in `.data` (for example OPENSEG memory-card messages via a table around `0x801f5440`–`0x801f5540`) | H |
| MSD scripts (`B:\BETA.MSD`, `C:\EVENT\unit0%d.MSD`, city scripts in `C:\area%2.2d.pak` kind 2) | `"MSCD"`, `u32 3`, `u32 size`, `u32 nregs`(?), then 4-byte-aligned VM records `u16 op, ...`. Text record = **op 8**: `u16 8, u16 reg, u16 len, char[len]` (NUL included, padded to 4). | VM `FUN_80021198(vm, regs)` stores a **pointer to the text in register `reg`**. A following command record (op 0x0A–0x0E: `u16 op, u16 cmd, {u16 is_reg, u16 value}×(op−0x0A)`) yields to the overlay, which reads the text through the register. Other ops: 5 jump (8 B, offset at +4), 6 raw block (4 + `u16` len), 7 arithmetic (12 B), 9 conditional skip (12 B). Record sizes: 0x0A 4, 0x0B 8, 0x0C 12, 0x0D 16, 0x0E 20. A walker using these sizes parses both `BETA.MSD` files end to end (JP 128 / US 127 text records), and the `UNIT0n.MSD` pairs (JP 207/185/153, US 233/207/153). | H (VM + walk), M (header) |
| Card DB `B:\CARD2.CDD` | fixed slots (see HYBRID §6): name 21 bytes at +0x03, attack names, effect text 4 × 19 bytes (US 4 × 21) | record pointer + fixed offset (`FUN_8003c0b0`) | H |
| Deck DB `B:\DECK2.DEK` | fixed slots | | M |
| Player name | player struct `+0x1ca` (SJIS, from name entry) | `strcpy` in the expanders | H |

MSD text uses the two-character `\n`. Overlay `.rodata` text uses real `0x0A` bytes. The renderer
accepts both. (H)

### 5.2 Concrete JP ↔ US pairs

| Example | JP location | JP bytes/text | US location | US text | Conf. |
|---|---|---|---|---|---|
| Menu label "Yes"/"No" (dialog default) | EXE `0x80010028` / `0x80010030` | `82 CD 82 A2` はい / いいえ | EXE `0x80010028` / `0x8001002c` | `Yes` / `No` | H |
| Main menu "no game file" (memory card) | OPENSEG `0x801e21ac`, pointer at OPENSEG `0x801f5524` | 「スロット1のメモリーカードに…ゲームファイルがありません！」 (3 lines) | OPENSEG `0x801e1b60` (referenced by code immediates, not a table) | `*s0MEMORY CARD in MEMORY CARD slot 1\ncontains no Digimon\nDigital Card Battle game data.` | H (strings), M (US reference) |
| Card name (card 0) | `CARD2.CDD` file offset `0x0B` (record 0 at 8, +3) | `83 43 83 93 83 79 …` インペリアルドラモン | `CARD2.CDD` file offset `0x0B` | `Imperialdramon` | H |
| Battle banner "…'s support card" (entry 14) | table `0x80070e78` + 14·4 = `0x80070eb0` → `0x80011834` | 「戦闘 P0の援助カード」 | table `0x8006e29c` + 14·4 = `0x8006e2d4` → `0x8001148c` | `Battle: *P0's Support Card.` | H |
| Tutorial line | `BETA.MSD` +0x52E, op 8, len 0x7D | 「レベルe3のカードを出すのが…」 | `BETA.MSD` +0x54E, op 8, len 0x6D | `It's best to start with *e3 Cards.…` | H |

The US MSD files keep the same record structure, and the text records grow or shrink in place. Only
the offsets after the first changed string move. (M)

## 6. Text windows and layout

| Item | Value | Evidence | Conf. |
|---|---|---|---|
| Line height | 13 px + `h` spacing. Glyph cell 12 high (JP 11 rows + shadow). | `y += 0xd + h` | H |
| JP advance | 12 px fixed, or ink width (min 8) in proportional mode, plus `w`. Space 6. | §3.2 | H |
| US advance | width-table nibble (2–6 px typical), fixed 6 with `*s0`. Space 4. | US table | H |
| Wrapping | **none** in any renderer. Callers never split lines either. | all draw functions | H |
| Generic dialog | width = measured width + 4 (even), height = measured height + 4 (+16 with choices), centred on 320×240. Grows with the text. | `FUN_80019ff0` | H |
| Tutorial box | width = measured width, centred horizontally, vertically centred on the `y` argument. **Expanded text is copied into a 144-byte stack buffer with no length check.** | `FUN_KAWSEG__801ed334` | H |
| Battle banner | one line at (16, 14), clipped by a draw area of 288×12 | `FUN_800466d0` | H |
| Battle help line | one line at x = 80, clipped at 16 + 288 = 304, so about 224 px usable | `FUN_80046a3c` | H |
| Card panel | name at +0x44 (1 line, ends before the icons at +0xBD, ≈121 px), effect text 4 lines at x+0x8E, stride 12. JP slots 19 bytes = max 9 kanji ≈108 px. | `FUN_8003c0b0` | H |
| Big name | JP max 8 characters (32 px each), US max 12 (16 px) | `FUN_80044684`, US `FUN_80041ca8` | H/M |

What longer English text breaks:

1. The tutorial buffer: the longest `BETA.MSD` text record is 125 bytes (JP) / 122 bytes (US),
   including the NUL. One name expansion can then pass 144 bytes. (H for the sizes, M for the
   overflow in practice)
2. Fixed-slot data (card effect lines 19 vs 21 bytes, names 21) is already covered in HYBRID §6.
3. Single-line clip areas (banner 288 px, help line ≈224 px) cut long strings silently. US lines were
   written for the same boxes, so this is only a risk for new translations. (M)
4. Glyph cache capacity is irrelevant for ASCII (the US renderer does not use the cache). (H)

## 7. Override plan

C overrides in `src/game/overrides/`, registered in `config/SLPS-03101/overrides.json`. The
recompiler rewrites every `jal` to an override symbol and keeps the original as `f_<addr>` (seen
for `dcb_task_sleep` in `generated/SLPS-03101/main_000.c`), so overlay call sites are covered too.

### 7.1 Detection rule

A string is "US-style" when it contains no byte 0x81–0x98 (no SJIS lead byte). Mixed strings do not
exist in either data set. (M: true for the strings sampled.) The overrides dispatch per string:
SJIS → original JP code, ASCII → the US-style path. An untranslated JP string therefore still renders.

### 7.2 Functions to override

| JP function | Override | Must do | JP path to keep |
|---|---|---|---|
| `8002ae00` draw | `dcb_text_draw(x, y, clut, prop, rgb*, ot, s)` | ASCII: port of US `80028d48`. `*` escapes, width table, SPRT from the US font rows in `SYSTEM.TIM`, JP globals (`9708/970c/9714`), JP CLUT base, JP OT/primitive pool, the `80029ef4` pool check. Return width. | SJIS strings → `f_8002AE00` |
| `8002b638` measure | `dcb_text_measure(prop, s)` | ASCII: port of US `800293fc`, honouring `prop`/`*s`. Set `g_text_w/h` exactly as the JP code does (relative height). | SJIS → `f_8002B638` |
| `8002bd3c` / `8002c574` condensed | same dispatch | ASCII: normal US font (US has no condensed ASCII) or a 1-px tighter spacing | SJIS → original |
| `80029f70` icon | `dcb_text_icon` | v base 0x7F instead of 0x69 **when the US `SYSTEM.TIM` is loaded**, and the US mode 3 (5×5) if US strings use it | original when the JP sheet is loaded |
| `800466d0` banner | `dcb_battle_banner` | expand `*P0/*P1` (US) instead of bare `P`, then draw | JP strings: original |
| `FUN_KAWSEG__801ed334` tutorial | `dcb_tutorial_msg` | expand `*p`, use a larger buffer, size the box with the ASCII measure | JP: original |
| OPENSEG slot expander (not yet located) | – | `*S` / `*E` | – |
| `80044684` big name | `dcb_big_name` | ASCII name → US `FONT.ARC` glyphs (16×32, max 12) | SJIS → original |
| `8002a37c` SJIS→mini | `dcb_sjis_to_mini` (done, §7.10) | keep ASCII letters (the JP version drops them), so English names show in the mini font | SJIS → original |

The US data use `*` for every code. JP strings keep bare codes. The dispatcher must therefore never
look for bare-letter codes in ASCII strings. (H)

### 7.3 Assets needed at runtime (from the player's US disc, never shipped)

* The US `SYSTEM.TIM` rows with the ASCII font (v 0x30–0x77), and the icon block if the icons are
  switched.
* The width table (US EXE `0x8006df9c`, 96+ bytes).
* `FONT.ARC` for the big name.

Option A: load the whole US `SYSTEM.TIM` and patch the icon base. The JP mini-font kana is then gone,
which only matters for untranslated JP strings drawn with `800288c8`. Option B: copy only the US
font rows somewhere the ASCII path can sample, and keep every JP path intact. B is what the port
does, but **not in VRAM**:

* The first try, (0, 320), sits inside the 320×480 framebuffers: the frame background painted over
  the font every frame. (H)
* The second, (512, 320)–(575, 495), looked empty in one survey, but 36 TIMs in the game load there
  (C.DRV city maps at (512, 256) 128×128 and others): a font there overwrites the game's art, or
  the game's uploads overwrite the font before the glyphs are drawn. (H, from a scan of every TIM)
* What the port does: the font rows (64 units × 176 rows, 4 bpp) live in a **private sheet in
  the GPU** (`hle::Gpu::set_private_sheet`), outside the 1024×512 VRAM. Each glyph packet puts the
  marker GP0 `EFD00000` (a NOP on the real GPU) where the JP code puts its texpage word; the GPU
  samples the following SPRT from the sheet at its UV (sheet row 0 = font row 48), with the palette
  still from VRAM (the JP text CLUTs). Nothing the game uploads can touch it, and it touches
  nothing of the game's. (H, `gpu.private_sheet` test)

Why not the JP full-width roman letters: the JP font has Ａ–Ｚ/ａ–ｚ (name entry uses
them), so English could be drawn by converting ASCII to full-width SJIS and keeping the JP
renderer. It is not used because of width: those glyphs are 8–12 px even in proportional mode,
the US font 2–6 px, and the US text was line-broken for the small font (an 18-character effect
line is ~100 px in the US font, over 160 px in JP letters, in a ~108 px box).

### 7.4 Risks

| Risk | Mitigation |
|---|---|
| The call sites with no static caller (`80044684` task entry, `8001a590` dialog callback) are reached through `function_table.c`. The override must also replace the table entry. | Check how `overrides.json` treats indirect calls before relying on it. |
| The expanders (`P`, `p`, `S/E`) run **before** the renderer and are not hooked by the draw override. | Override each expander (§7.2), or pre-convert JP strings. |
| Text measured with one renderer and drawn with another gives a wrong box size. | Always dispatch measure and draw with the same rule. |
| Cache side effects: JP measure inserts glyphs. The ASCII path must not touch the cache. | – |
| Primitive pool (1000 per frame): ASCII text uses the same number of SPRTs, so no change. | – |
| Overlay functions (`KAWSEG::801ed334`) as override targets: support is unverified. | Check the override loader for overlay addresses. |

### 7.5 Test plan

| Scene | What to check |
|---|---|
| Boot → main menu with no memory card | the "no game file" dialog (OPENSEG), box size, 3 lines |
| Main menu, save/load, slot messages | `*S`/`*E` expansion, `*s0` fixed width |
| Tutorial (Practice → Tutorial, `BETA.MSD`) | line breaks `\n`, `*e3` icons, `*p` name, box size, the 144-byte buffer |
| Battle: banner and help line | `*P0/*P1`, clipping at 288 px |
| Battle: card info panel | card name, attack names, 4 effect lines, `*e` icons |
| City (SAISEG) event dialog, `area00.pak` | MSD op 8 via the SAISEG host |
| VS screen | big-name font |
| Any untranslated JP string | still renders through the JP path |
| Headless regression | `DCB_HEADLESS=1 DCB_FAST=1 DCB_SNAPSHOT=…` frame dumps at fixed frames with the pad script, compared before and after |

### 7.6 Implementation status

| Item | Status |
|---|---|
| The four dispatch overrides (`dcb_text_draw`, `dcb_text_measure`, condensed pair) | **done** — `src/game/overrides/text.cpp`, registered in `config/SLPS-03101/overrides.json`. The original stays compiled as `f_<addr>` and runs for SJIS strings or when `en_font.bin` is missing, so untranslated JP still renders. |
| Shared parser (`*` codes → glyphs/icons/newlines) for measure and draw | **done** — one `run_string`, so a string can never be measured with one path and drawn with another. |
| Font + width-table assets | **done** — `tools/text/en_text.py` writes gitignored `assets/SLPS-03101/en_font.bin` (sheet rows 48–223 + the 96-byte width table from `0x8006DF9C`); the header is checked at load and a wrong file falls back to JP. |
| Font placement | **done** — the GPU's private sheet, outside VRAM (see §7.3). |
| Card/deck text | **done** — the converter grafts US names/attack names/effect lines into `CARD2.CDD`, deck/owner names into `DECK2.DEK`; lines too long for the JP slot are listed in the local `en_text_report.txt`. DEK field layout measured from both dumps: deck JP 13 / US 19 B at +60, owner 21 B at JP +73 / US +79, 10-byte tail at JP +94 / US +100. |
| English inside JP strings | **done** — the game appends `デック` to a deck name (`"%sデック"`, EXE `0x800114E0`; the US has `"%s Deck"`) and drops names into JP messages. Each string is copied to the host and cut into pieces: English runs (two letters in a row outside bare JP codes, or one letter the JP renderer would skip: a capital or a lowercase letter that starts no JP code, e.g. a name typed on the ABC page next to kana) go to the US port, the rest to the JP original on a NUL-terminated copy pushed on the guest stack. The format itself is patched to the US `"%s Deck"` when `en_font.bin` loads (stock bytes only), so JP-named decks read "… Deck" too; three more "%sデック" copies live in overlays (P.DRV), so a string ending in `デック` (no newline) also gets `" Deck"` (`"Deck"` alone on the name-entry screen), leaving the name before it as typed; `デック` right after an ASCII character in other strings also becomes `" Deck"`. |
| Deck names longer than the 13-byte slot | **done** — 48 US names (e.g. `Mountain CrusherDX`, 18 letters). The slot holds the first 11 letters + a tag byte (1, 2… per shared prefix: `Mountain Crusher` and `Mountain CrusherDX` both start `Mountain Cr`); `en_names.txt` maps the key to the full name, which the renderer draws (and measures) wherever the key appears. Renderers not taken over yet show the 11 letters. |
| VS-screen big names, record strip | **done** — §7.11. |
| Tutorial and Fusion Shop scripts | **done** — §7.12. |
| Battle banner / slot expanders, the five extra renderers, EXE + MSD strings | not started (later milestones). |
| Verification | headless runs with `DCB_TRACE_TEXT=1` + `DCB_SNAPSHOT`: an ASCII message (poked into the memory-card dialog source at OPENSEG `0x801E27F4`, draw buffer `0x800E5638`) renders through the US port, and a clean run of the same build renders the JP string through the fallback. |

### 7.7 Text catalog (whole strings, any language)

Strings the game draws whole (menus, dialogs, the save/load screens) are translated by template,
not by patching the overlays: the renderer copies each string, and when it matches a catalog
entry whole, draws the translation instead.

- **`config/SLPS-03101/text/catalog*.txt`** (in git, offsets only; one file per area, all read): `pair <id> <us id>` and
  `run <id> <us id> <count>`, ids being `<file>:<offset>` in the JP / US disc (`EXE` = boot.exe,
  `OPENSEG` = P.DRV OPENSEG.BIN, ...). A `run` pairs strings in a row by order, for blocks the
  two versions keep in the same order (the 27 save/load messages, the 33 location names).
- **`config/SLPS-03101/text/en*.tsv`** (in git): English written for this port where the US has
  none (`pair <id> -`), e.g. `%3dh %2dm` for the JP play time.
- **`tools/text/en_text.py`** reads both dumps and writes `assets/SLPS-03101/text/source.tsv`
  (the JP templates) and `en.tsv`.
- **Templates:** printf placeholders the game fills (`%d`/`%3d` match padded numbers, `%s` any
  run, `%%` a literal %); the slot digit the game writes over `S`/`E` after `スロット` (US: over
  `*S`/`*E`) is `%c`, and the card count OPENSEG `801EA4F4` writes over `??枚` (" n" / "nn", the
  old-save conversion's "??枚のカードデータの修復に成功しました。", OPENSEG:494,
  `catalog-reception.txt`) is `%2d`. Captured values go into the translation's placeholders in order.
- **One JP string, two meanings:** the catalog has one translation per JP text (the lowest id's).
  OPENSEG draws its choice labels みる / みない (801E1888 / 801E1890) for the Polygon Battle
  setting and for the new-game "see the explanation?" prompts ("User Registration is complete.
  Would you like to know more about this world?", "Do you want to learn about the game?"); the US
  split them (a7c "Yes"/"No", ab4 "On"/"Off"), but here みる/みない took KAWSEG:1a48/1a50 "On"/"Off"
  (the battle option) everywhere, so the registration prompts offered On / Off. `dcb_dialog_setup`
  (override of `dialog_setup` 80019FF0, `src/game/overrides/dialog_labels.cpp`) gives a choice box
  with no text and those labels the default labels はい / いいえ (80010028 / 80010030, "Yes" /
  "No"); the Polygon Battle box has a title and keeps On / Off. Only with the English assets.
  (H: headless new game, `DCB_TRACE_TEXT=hex`: the prompt box draws 80010028/80010030 → Yes/No.)
- **Other languages:** a `<lang>.tsv` with the same ids next to `en.tsv`, picked with
  `DCB_LANG=<lang>`. The US font has ASCII only, so accented letters need glyphs first.
- **Runtime:** `text::Catalog` (`src/platform/text_catalog.*`, unit-tested), used by
  `load_text` in `src/game/overrides/text.cpp` before the deck-name rules.
- **Typed-out messages:** many message windows reveal a line a Shift-JIS character per frame
  (the new-game guide, for one), drawing each partial string. `translate_prefix` matches a
  partial string against the start of a template (it must end inside a literal part, so a
  template starting with `%s` does not take everything) and draws the same share of the English:
  k of n Japanese characters shown -> k/n of the English glyphs, so the English types along
  instead of the Japanese showing until the line completes. While the start could still be two
  different messages it draws nothing. `DCB_TRACE_TEXT=hex` logs `catalog=prefix -> "..."`,
  and `out=jp|en` + `outhex=` (what is drawn after every expansion: `out=jp` is Japanese on
  screen, even for a line the catalog "translated").
- **Templates with a literal start first** (`Catalog::lookup`, used by `load_text`): a whole
  template that starts with a literal, else the start of one (typed out), else a whole
  `%s...` template (`%sデック`, `%sの...`), else the start of one. A line typed out passes
  through a whole `%sデック` for a frame: the partner select's deck descriptions reach
  `ブイモンがパートナーのc5デック` before `c7です。...`, and were drawn for that frame as
  "ブイモンがパートナーのc5 Deck" (Japanese flash, twice per description; also the starter-deck
  line before it). `%s` captures also end on a character boundary: byte-wise, `ブイモ` (ends
  `83 82`) "started" `%sの...` (`の` = `82 CC`) and drew nothing for that frame. (H: run
  headless, new game → partner select, all three decks: no `out=jp` draws besides the typed
  name and the kana grid.) Two messages that start alike (`パートナーカード...`, `この世界...`)
  still draw nothing for the few characters they share.

Coverage (1271 strings): Yes/No and the save/load flow (`catalog.txt`), the EXE (battle
dialogs, banner, help lines, support effects, packs, rank titles: `catalog-exe.txt`), KAWSEG
(battle overlay: deck select, option effects, experience / Digi-Parts screens, pause menu,
result bonuses), SAISEG + SUBSEG (player data, Digi-Parts, card list, deck edit, auto deck),
OPENSEG (registration, partner / starter select, trade, battle with a friend), EVOSEG (fusion)
and ENDSEG (records, titles). SUGSEG has no Japanese text.

Rules the mappers followed, worth keeping:
- **Confirm button:** this build confirms with ○; the US moved the confirm/cancel icon codes
  (`b0`/`b1`/`b2`) in menu hints. Hints with those icons use port-written English with the JP
  codes (`en-*.tsv`); attack icons (the same on both discs) keep the US text.
- **Not translated:** input grids (kana tables), secret keywords the game compares with what
  the player types, strings the game overwrites in place beyond the slot digit ("??").
- A `%s` the game fills keeps its (JP) value inside the English line; a title or deck name
  there stays JP until its own source is translated.

### 7.8 City scripts (C:\AREAnn.PAK)

Each city PAK has two chunks: kind 2 (id 0xC8+n) the city MSD script, kind 5 (id 0xFA0) the image
set. The US scripts are the same program as the JP ones: walked record by record
(`tools/text/msd.py`), they differ only in text records and the show-text commands (op 0x0A,
cmd 4/5) around them, where the US re-flowed its dialogue. `en_text.py` checks that per city
(`msd.same_program`) and writes `files/C/AREAnn.PAK` with the US script chunk (the image chunk
stays JP).

The JP city code cannot show those lines as is: `city_msg_build` (SAISEG 801E2978) copies a
line into one of three 64-byte slots at `g_city_msg_lines` (801F7E88) keeping only Shift-JIS and
bare JP codes (all ASCII is dropped) and without a length check (the US lines reach 67 bytes).
`src/game/overrides/city_text.cpp` overrides it and `city_msg_reveal` (801E2B94): an English line
(or a JP line the catalog translates) stays host-side, the slot holds a marker and a serial, and
the reveal types two characters a frame through the text renderer (the shown count stays in the
slot, so save states keep their place). JP lines take the originals.

Not covered yet: the other MSD scripts (E/F/C PAK scripts; the tutorial and the Fusion Shop are
§7.12); the other renderers
(the tiny font; the mini font is §7.10, the VS big names §7.11). `DCB_TRACE_TEXT=hex` logs each drawn string's bytes and
whether the catalog translates it (decode with cp932) — the way to find what is still Japanese.

Japanese that is not text (no text-engine call; found with `DCB_TRACE_PRIMS=1` + `DCB_LOG_TEX=1`):
the battle phase banner's 準備 / 進化 / 戦闘 (B:\CBTL_SYS.ARC TIM #17 @0x11240, 44×72 4 bpp at
VRAM (948, 304), CLUT (816, 497); the US TIM reads Prep / Digi-volve / Battle, but the HD pack's
`B_CBTL_SYS_off00011240_44x72_pal0.png` replaces it with the JP art), and the world map's HELP MENU
plate (移動 / 入る / メニュー: a TIM of the C:\area01.pak image chunk, HD key
`C_OBJECT_WORLD_off0000b51c`, 88×80 4 bpp at (808, 0), CLUT (528, 242), replaced by
`C_OBJECT_WORLD_off0000b51c_88x80.png`).

### 7.9 Counts and units on the deck / card screens (SUBSEG)

The JP screens draw a count with the 6×11 digit font (`80028C84`, `sprintf "%2d"/"%3d"/"%4d"`)
and then a one-kanji unit with the main renderer at a fixed x: 枚 (cards), 戦 勝 敗 (battles,
wins, losses), 計 (total). The US build moved those draws and drew the units in a **4×5 capital
font** that the JP build does not have: US `8002790C` (grey wrapper of `8002793C`) and
`80027DB8`, SPRT 4×5, advance 4, lower case folded to upper, u = (c & 15)·4 (+64 for
`80027DB8`), v = 5·((c − 0x20) >> 4) − 22 in the SYSTEM.TIM page (TIM rows ≈ 234–253, outside
`en_font.bin`). The counts use the main font with `*s0` (fixed 6 px). US overlay base: SUBSEG
loads at **`0x801DDF38`** in the US build (not `0x801E0B30`; the US EXE is smaller), so US
string `SUBSEG:x` is at `0x801DDF38 + x`. (H, US disassembly)

| Screen (JP function) | JP draws (x from the panel origin x0) | US draws (function) |
|---|---|---|
| Deck select, deck line (`801EEBC4`) | `%4d` +106, 戦 +132, `%3d` +156, 勝 +176, `%3d` +200, 敗 +220 | `*s0%3d` +98, tiny "Battles" +118, `*s0%3d` +149, tiny "Wins" +169, `*s0%3d` +189, tiny "Losses" +209, all tiny at y+7 (US `801EB3EC`–`801EB500`) |
| Deck select, type rows (6 columns, step 59, second row +13) | type label +0, `%2d` +28, 枚 +44 | `*a0`… icon +0, `*s0%2d` +16, tiny "Cards" (`80027DB8`) +32; the option column: "Option Card" +0, count +58, "Cards" +74 |
| Deck select, partner | label +138, `%2d` +205, 枚 +221 | "Partner" +160, `*s0%2d` +193, "Cards" +209 |
| Deck select, level row | Ｌｖ +0, level icon +16, `%2d` +36, 枚 +52; next level +84, +85 | "Lv" +0, icon +12, `*s0%2d` +30, "Cards" +46; next +81, +82 |
| Deck edit side panel (`801F32BC`, x0 = 10) | icon +0, `%2d` +16, 枚 +31; total row 計 +0 | icon +0, `%2d` +21, tiny "Cards" +35 (y+6); total row tiny "Total" +0 (US `801EF900`–`801EFC44`); the partner page draws "Pa" (main font) |
| Card list rows / card info / top line | 枚 at 291 (rows); ` 計` +84 before a `%4d`; 総枚数 at 242 before the total | "Total Number" / "of Cards" in two lines, `*s0%4d` |
| Sort menu | one string per entry, clipped at about 74 px from the entry's x | same strings ("*b2 Attack Power", "Level *e3" …) — the US menu is wider |

The port keeps the JP positions and fits the English with short forms (`catalog-layout.txt`,
`en-layout.tsv`): 枚 → `Cds` (with `*w-1`, 13 px: the deck edit panel, the card list rows and
the deck select columns leave 13–15 px), 戦 → `Btl.`, ＯＰ → `Opt.`, 計 → `Tot.`, 総枚数 →
`Total`, 攻撃力 → `Atk. Power`, 最新入手 → `Newest`, 所持枚数 → `Cards Owned`; the sort
menu's levels are spelled `Level R` / `C` / `U` / `A`. A JP string with several catalog entries
takes the entry with the lowest id (`std::map` order of the ids), so a change goes on that id.

**Baseline.** Every one of these labels sits on the line of a count in the 6×11 digit font, and
both are drawn at the same y: the digit font draws at y + 1 and its digits fill 10 rows (y+1 ..
y+10); the JP kanji filled y .. y+10 around them, but the US capitals fill the top 10 rows of their
cell (y .. y+9), so every English unit sat one row too high ("W"/"L" most visibly). All the unit
labels of `en-layout.tsv` (Cds, Btl., Opt., Tot., Total) and 勝/敗 (`en-kawseg.tsv`: KAWSEG:360/364,
the lowest id of that text, so every screen's win/loss unit) start with the port code `*y1`
(§2.2), which puts the letters on the digits' rows. (H: pixel rows of the card-info W/L and the
card list's Tot./Total/Cds in headless snapshots, before 63–72 vs digits 64–73, after 64–73.)
**Spacing.** A count ends 2 px before its unit's x (`%3d` +156 → 勝 +176 on the deck line, the same
20-px step in KAWSEG / OPENSEG / SAISEG / the card info): the kanji's ink kept a gap there, "W" did
not ("5W"), so W / L get a leading space ("5 W"). The card info's " 計" (25 px before a `%4d`)
became "Tot." without the space: " Tot." was 24 px and touched a 4-digit total.

**Level badges.** Icons 16–19 (mode 0) are the level badges: JP Ⅲ, A, Ⅳ, 完; the US grid (v base
0x7F) has R, A, C, U there. Those US rows are inside the private sheet (TIM rows 48–223), so
`dcb_text_icon` (`src/game/overrides/level_badges.cpp`, override of `80029F70`) lets the game
emit the icon and points the packet at the sheet (texpage word → sheet marker, v → US row). Every
game draw of a level badge goes through `80029F70`; the `*e` codes inside English strings do
not (text.cpp calls `f_80029F70` directly), so they keep the JP art.

### 7.10 Mini font (8×7) in English

`src/game/overrides/mini_text.cpp` overrides `sjis_to_mini` (`8002A37C`) and `text_draw_mini`
(`800288C8`; the grey wrapper `80028898` reaches it through its rewritten `jal`). (H: run)

- **`sjis_to_mini`** is only called on card names (card+3): battle card panel `8003C200` (dst: a
  40-byte stack buffer) and SUBSEG `801E6AC4` / `801E78A4` (sp+32, ≥ 64 bytes). Its result is
  unused. An ASCII name is copied through, bounded to the 20 letters of the name slot; bytes below
  0x20 (long-name tag) and `~` (kana bank toggle) are dropped; US `*a`–`*e` + digit (item names
  such as "Defense Disk *b0") become the mini icon code `01 N` with the original's mapping
  (`b0` → `01 08`). Shift-JIS takes the original.
- **`text_draw_mini`**: the string is translated whole by the catalog (a bracketed label "(%s)",
  the partner screen's support label, is translated inside the brackets). Japanese left (a byte
  ≥ 0x80 or `~`) → the original. A string with lowercase letters → **proportional**: one call of
  the original per glyph on a 2-byte guest-stack string, each glyph trimmed to its ink columns
  (measured from the SYSTEM.TIM mini cells in VRAM), 1 px gap, space 3 px, no space after an
  icon; `01 N` icons through `dcb_text_icon`. Capitals only (the JP disc's "RANK UP!",
  "L1      ", translated "FULL SET!") keep the fixed 8 px cells, since callers pad with spaces.
- Why not the micro font: the US drew these in a 4×5 capital font (US `80027DE8`; JP has the same
  cells at `80027EF4`, no lowercase). The mini font already has full ASCII with lowercase; with
  ink widths English fits the JP slots, and 7-px letters read better than 4×5 capitals.
- Mini ink widths: capitals and digits 7, most lowercase 5 (`a`, `d`, `g` 6), `i` 1, `l` 2.
  Slots: support label ≈ 48 px (the specialty icon 49 px after it in the battle panel; deck
  edit's effect text at +52). In that budget the US "1stAttack" (56) and "Eat-up HP" (57) do not
  fit: `en-mini.tsv` shortens them ("1st Atk", "Eat HP"); "\x01\x08 Counter" is 51 with the
  icon. (H: measured)
- **Card-name slots** (widths as the trace's `w=`, the pen advance with the last glyph's 1 px
  gap; measured on headless snapshots, H: run):

  | `sjis_to_mini` caller | screen | name x | slot | limit |
  |---|---|---|---|---|
  | `8003C200` | battle card panel | panel + 32 (175 at rest) | 88 | the field ends at 263, the DP box border at 264; ink to 261 |
  | SUBSEG `801E6AC4` (`801E65E8`) | Edit Partner, partner panel | 59 | 144 | panel inside ends at 203 |
  | SUBSEG `801E78A4` (`801E73BC`) | Edit Partner, armor panel (a Digimon card; shown when partner record +770 ≠ 0) | panel + 3 = 217 | 86 | panel inside ends at 303 |

  At normal spacing 17 of the 301 US card names are wider than the battle slot, 24 than the
  armor slot, none than the partner slot. `dcb_sjis_to_mini` knows the caller by its return
  address and fits the name (`src/game/overrides/mini_fit.hpp`, `fit_name`): the full name if
  it fits; else the full name in **tight spacing** (1 px between two glyphs only where their
  facing ink columns share a row, else 0; 2 px between words), which takes 8 of the 17
  (MetalSeadramon 89 → 85, PlatinumSukamon 89 → 86, Mega Rec. Floppy 91 → 85, Dark Lord's Cape
  96 → 87, the three "Mega Def. Disk" 89 → 82, Cherrymon's Mist 90 → 86); else our **short
  name** from `config/SLPS-03101/text/short-names.tsv` (card number, short name, full name as a
  check; `tools/text/short_names.py` → `assets/<serial>/en_short_names.txt`, full → short), in
  normal spacing, or tight when only that fits. The other 9: RealMetalGreymon → R.MetalGreymon,
  MasterTyrannomon → MstrTyrannomon (tight), HerculesKabuterimon → HrcKabuterimon,
  MegaKabuterimon → M.Kabuterimon, Mega Attack Chip → Mega Atk. Chip, Another Dimension →
  Another Dim., Download / ArmorCrush / De-Armor Digivolve → … Digi. Dropping the gap
  everywhere (0 px) would fit all but two, but merges letters ("rn" reads "m"). The spacing is
  handed to the draw that follows (the same buffer and text), not written into the string. The
  short names are used only there: the main font's card info and lists keep the US name. The
  armor slot only shows Digimon, so the items over 86 px (Dark Lord's Cape, tight 87) never
  land there. `DCB_TRACE_TEXT` logs each fit as `[text] mini-name <slot> src=… "full" ->
  "drawn" normal|tight w=N`, and the draw as `mini tight w=N`.
- Strings seen through the mini path (headless runs): the Deck Select HELP legend (SUBSEG
  1ac8.., catalog-subseg), the deck edit sort hint (SUBSEG 1c88), the support labels (EXE
  2320.., catalog-exe) in deck edit / card selection / partner / battle, battle card names.
  `DCB_TRACE_TEXT=1|hex` logs them as `[text] mini jp|prop w=N|fixed`.

### 7.11 VS-screen big names

Before a card battle the VS screen (`vs_screen`, KAWSEG `801F2B9C`) shows both names in 32-px
letters. It spawns the task `vs_names_load` (KAWSEG `801F04FC`, a0 = mode, a1 = match number,
a2 = the VS task) and keeps running; the drawer (`vs_screen_draw`, `801F35D8`) draws each name
as one sprite from VRAM (704, 448) (row 0) / (704, 480) (row 1), CLUT (752, 471 / 472), width
`battle_data+0x114`. (H: read in code, run headless)

| Step (`vs_names_load`) | What it does |
|---|---|
| names | mode ≠ 0 (a city/arena opponent): row 0 only, the player. Mode 0 (battle with a friend): rows 0 and 1, and match = 999. For each: `task_spawn` of `bigname_load(*g_game_data + row·10040, row, self)`, then `task_sleep(0x7FFFFFFF)` until it wakes the parent. |
| `bigname_load` (`80044684`) | `g_bigname_busy` = 1; for each 2-byte character (max 8): sprintf `B:\FONT\%04X.tim` (EXE `800117C0`), `task_spawn(file_load_task)`, sleep, `tim_upload(tim, 704 + i·8, 448 + row·32, 752, 471 + row)`, `DrawSync(0)`, `mem_free`; `g_bigname_busy` = 0; `task_wake(parent, leftover a1)`. |
| opponent picture | sprintf `B:\MATCH\%3.3d.ARC` (KAWSEG `801E0E1C`), load it, `tim_upload(entry, -1, -1, -1, -1)` for **every** offset-table entry (the last one is the end of the file: ReadTIM fails and the previous TIM is uploaded again), sleeping `g_wait_frames` after each. The 14 TIMs are the portrait, frames, the 戦 勝 敗 record strip (464, 184/202), and last the opponent's name picture at (704, 480), CLUT (752, 472), JP 32 halfwords (128 px). |
| widths | `battle_data[0]+0x114` = (strlen(`+0x1CA`)/2)·32; `battle_data[1]+0x114` = `g_tim_image.prect->w`·4 (mode ≠ 0) or the same formula (mode 0). |
| end | four slide-in records at `g_vs_slide` (801FE788; the picture enters from −width), `task_sleep(10)`, `g_vs_names_busy` (801FF1E8) = 0, `task_wake(parent)`. |

Name sources (run headless, Meramon in the Flame City Battle Café): `*g_game_data` = 800DF1C4
(the loaded save's name, +0), `battle_data[0]+0x1CA` = 800E61C2 holds the same name (a battle
copy); the opponent's `+0x1CA` (800E63A2; presumably its DEK owner name, not checked) is not used here: the VS screen shows the
MATCH picture instead. **The player name is typed on the JP kana grid, so it is Shift-JIS**; an
ASCII name comes from outside (a US save, a cheat) or, in the port, from the name entry's ABC page (half-width letters, docs/re/name-entry.md).

JP behaviour with an ASCII name: `%04X` of two ASCII bytes asks for files such as `FONT\4A6F.tim`
that do not exist, the load task wakes with NULL and `tim_upload(NULL, …)` uploads garbage; an odd
length pairs the last letter with the NUL and reads past it; the width formula drops the odd
letter.

**Port** (`src/game/overrides/bigname.cpp`, `tools/text/bigfont.py`):

- `en_text.py` → `bigfont.write_assets` writes `en_bigfont.bin` (the US FONT.ARC repacked: "BGF1",
  first 0x20, count 92, 4 halfwords × 32 rows, the CLUT, a has-glyph byte per code, 92 × 256 B of
  pixels; a code whose TIM belongs to another code has no glyph) and `files/B/MATCH/NNN.ARC`: the
  JP archive with only its last TIM (the name picture) taken from the US archive — "Meramon",
  40–64 halfwords wide. 141 of 142 archives; `999.ARC` (mode 0) has no name picture and stays JP.
  The rest of the US archive is not used (its record strip is blank and wider: the US draws those
  words as text).
- `dcb_bigname_load` (override of `80044684`): a name made only of 2-byte Shift-JIS characters
  takes the original. Anything else is drawn with the US glyphs: ASCII as is, full-width
  letters/digits/space in a mixed name mapped to ASCII, bytes < 0x20 (DEK tag bytes) dropped,
  codes without a glyph drawn as a space, at most **12 characters** (the CLUTs at x 752 end the
  strip, as in the US loader). Each glyph is a 320-byte TIM built on the task's stack and uploaded
  with the game's `tim_upload(tim, 704 + i·4, 448 + row·32, 752, 471 + row)` + `DrawSync(0)`;
  `g_bigname_busy` and `task_wake(parent, 1)` as the original. Without `en_bigfont.bin` an ASCII
  name uploads nothing and gets width 0.
- `dcb_vs_names_load` (override of KAWSEG `801F04FC`): the routine re-implemented step by step
  (same spawns, stack arguments, sleeps, uploads, globals and slide-in records) with one change:
  an ASCII name's width is 16 px per drawn character. A wrapper around the original cannot do it:
  the VS screen draws concurrently and would use the JP width during the task's last 10 frames.
- Verified headless (`DCB_TRACE_TEXT=1` logs `[bigname] ...`): Meramon's VS screen with the JP
  save name ああああああ (original path, width 192, unchanged) and the US "Meramon" picture;
  with a cheat writing the save name, "Decky" (odd length, width 80) and "Tai.Kamiya-20"
  (`.` drawn as a space, cut at 12, width 192). Mode 0 (battle with a friend) not run.
- **Record strip** (戦 勝 敗, one picture in all 142 MATCH and 142 WIN archives, VRAM 464,184,
  136×18): `vs_record_draw` (KAWSEG `801F03C8`, a0 x, a1 y, a2 wins, a3 losses; called by
  `vs_screen_draw` for both players and twice by the result screen, near `801F6278` / `801F6524`)
  draws `%4d` (wins + losses), `%3d`, `%3d` with the digit font (`80028C84`, CLUT 7, OT 1) at x + 8
  / 56 / 97, y + 3, then the strip; the kanji are in the picture at columns 39–50 / 80–92 /
  120–131. The US counterpart (US KAWSEG `801ED968`; US overlays load at `801DDF38`) draws a blank
  192-px strip, `*s0%4d        %3d      %3d` in the main font at (x + 8, y + 3) and "BATTLES" /
  "WINS" / "LOSSES" in its 4×5 capitals (`80027DB8`, CLUT 6) at x + 36 / 102 / 156, y + 9. Port:
  `swap_us_images.py` cuts the US strip to 136 px (`NARROW`: it equals the JP strip without the
  kanji) and `dcb_vs_record_draw` (`src/game/overrides/vs_record.cpp`) runs the original, then,
  when the strip's kanji cells in VRAM are plain background, draws the catalog's English for
  戦 / 勝 / 敗 (else "Btl." / "W" / "L", the deck select line's words; without English assets the
  kanji) through `text_draw_grey` at x + 37 / 79 / 119, y + 3, CLUT 4 (the kanji's green). With the
  JP strip loaded it adds nothing. Verified headless on Meramon's VS screen ("1 Btl. 0 W 1 L",
  "5 Btl. 5 W 0 L"); the result screen was not run.

### 7.12 Tutorial and Fusion Shop scripts (B:\BETA.MSD, C:\EVENT\UNIT0n.MSD)

The other two MSD hosts besides the cities (§7.8). `tools/text/scripts.py` (hooked into
`en_text.py`) writes the US scripts as loose files `files/B/BETA.MSD` and
`files/C/EVENT/UNIT0n.MSD` (the loader upper-cases the path, so `C:\EVENT\unit00.MSD` finds
`UNIT00.MSD`), each only when `msd.same_program` passes for that host. The loose
`C:\EVENT\CITYnn.MSD` are unused copies of the city scripts in `AREAnn.PAK`.

**Hosts** (H: decompiled, and run headless with the grafted scripts):

| Script | Loader | Host (VM step) | Text commands | Other host facts |
|---|---|---|---|---|
| `B:\BETA.MSD`: the new-game practice battle with Betamon (`Tutorial Deck` vs `Practice Deck`) | KAWSEG `801ED178`, from the battle set-up `80042800` when `*(80070C30)+4` is 0 | `tutorial_script_step` KAWSEG `801ED5A8` | `0x0A` cmd 0: `tutorial_msg_show(reg 8, reg 0)` (box, waits for circle); cmd 3 copies reg 0 into a player's deck name ("Practice" / "Tutorial": both fit) | registers 4-7 = pad pressed this frame: circle (0x20), square (0x80), triangle (0x10), cross (0x40); a prompt polls them every frame with `op 9` tests |
| `C:\EVENT\unit0%d.MSD`: Andromon No.1-3 at the Fusion Shop | `unit_msd_load` EVOSEG `801EB200` (sprintf of `801E1D90`), from the shop entry `801EB980` | `fusion_shop_host` EVOSEG `801EACBC` | `0x0A` cmd 0: `unit_msg_build(reg 4)` adds a line to the page (4 lines; the first, the speaker's name, shows at once); `-1` (page full) makes the shop wait for circle, clear the page and add the line again (`801EB6D0`); cmd 11: wait for circle, then clear the page | the camera/model VM of EVOSEG (`801F1134`, `801F20F4`) has no text |

**What the JP code does to English**, and the overrides (`src/game/overrides/event_text.cpp`):

- `tutorial_msg_show` (KAWSEG `801ED334`) copies the text into a 144-byte stack buffer and
  replaces **every bare `p`** with the player name, then sizes the box with `text_measure`, opens
  the "TUTORIAL" window `801FF1A0`, points the tutorial VM's `+0x10` at the buffer (the draw
  callback `801ED2D8` draws it every frame) and loops until circle. In English every `p` of a word
  became the name, and the US `*p` left its `*`. `dcb_tutorial_msg_show` runs the same steps for an
  English line with `*p` expanded, in a buffer as long as the line (never shorter than the
  original's); JP lines (the catalog's first-turn lines, KAWSEG `0x428` / `0x44C`, come through
  here too) take the original. A JP name inside the English line is drawn by the mixed-string
  path of `text.cpp`.
- `unit_msg_build` (EVOSEG `801EBF4C`) claims a 64-byte line of `unit_msg_lines` (`801F7CF0`, 4
  lines; `+60` s16 shown, `+62` in use, `+63` length; `unit_msg_slot_alloc` `801EBEC0` sets shown
  to -1 on the first line), writes `w1`, copies Shift-JIS and the bare codes `a b c e s` + digit,
  expands `h0`-`h3`, and **drops every other byte** (all English), and marks a line of 60 bytes or
  more unused. `unit_msg_reveal` (`801EC33C`) shows one Shift-JIS character per call. The
  overrides `dcb_unit_msg_build` / `dcb_unit_msg_reveal` do what `city_text.cpp` does (shared
  helpers in `typewriter.hpp`): the English line, with `*h0`-`*h3` expanded as the JP does (the US
  uses `*h1`: "a(n) *c5*h1 Card*c7!"), stays host-side, the line holds a marker and a serial, and
  the reveal types two characters a frame; the name line shows at once, as in JP.

**Script check** (`msd.same_program(jp, us, show_text, button_regs)`). The check compares
*skeletons*: with every text-side record (op 8 and the host's show-text commands) taken out, the
two record lists must be equal (jumps by kind). The first version aligned the two record lists
with difflib, which failed on UNIT00/UNIT01: the US re-paginated whole scenes and the aligner
paired the repeated page structures wrongly, although the skeletons are identical (232 / 216
records). Text-side commands per host (from the host code above): cities 0x0A cmd 4/5; tutorial
0x0A cmd 0; Fusion Shop 0x0A cmd 0 and cmd 11 (the page break: the US moved them with the text).
Text records must load the same registers (tutorial 0, Fusion Shop 4).

**Buttons in the tutorial.** `BETA.MSD` differed in 24 button tests: the US script tests other
pad registers at the same places, with the same jumps. Paired prompt by prompt, US cross = JP
circle (confirm), US triangle = JP cross (discard the hand), US circle = JP triangle (skip); the
attack choices (circle, triangle, cross) are the same on both discs. `button_regs` = {4..7} lets
an `op 9` test of a pad register differ, and the graft writes the JP test back, so the tutorial
follows the JP controls (checked headless: circle confirms, cross redraws, square shows the hand,
triangle skips). The icons in the prompts are remapped the same way (`TUTORIAL_BUTTONS`: `*b2`→`*b0`,
`*b1`→`*b2`, `*b0`→`*b1`; icons b0/b1/b2/b3 = circle/triangle/cross/square), per stretch of text
between two program records: a stretch whose US icons already equal the JP ones (attack prompts
such as "I chose *b0") and lines about attacks ("Attack") stay; 21 lines change. The stretches
that still differ from the JP afterwards are listed by `en_text.py`; all are rewordings (the US
names the button where the JP says "a button", or the other way round).

**Other text fixes.** The US Fusion Shop text writes a double quote as the six characters
`\0x22` (6 times in UNIT00); no renderer decodes it (the US game shows it as is), so the graft
writes `"` in place (NUL-padded: no record moves).

**Verified headless** (`DCB_TRACE_TEXT=hex`, snapshots): UNIT00 (Flame City, from the player's
save: City menu → Fusion Shop): pages, typewriter, the name line, `"Partner Fusion."`, level
icons, and a fusion's "Wow! You got a(n) Black Gear Card!" (`*h1`); the tutorial (new game → name,
partner, save → Beginner City Battle Cafe, Betamon) up to the Digivolve phase of the second turn,
with a Shift-JIS player name ("ああああああ, now you know which Attack to choose, right?"). UNIT01 /
UNIT02 (Andromon No.2 / No.3 in other cities) were not reached: they pass the same check and run
on the same host code.

## 8. Proposed names

For later import into Ghidra / `config/SLPS-03101/functions.json`. Not applied in the shared database.

| Address | Proposed name | Purpose |
|---|---|---|
| `80015728` | `game_boot_main` | boot: inits the text system, makes glyph cache page 15 |
| `800270d4` | `text_system_init` | SYSTEM.TIM, CLUTs, fonts, primitive pool |
| `8002745c` | `text_prim_reset` | per-frame primitive pointer reset |
| `8002d564` | `kanji_font_load` | loads KANJI0/1.FNT |
| `8002d130` | `kanji_glyph_addr` | SJIS → 22-byte glyph pointer |
| `8002d1c0` | `kanji_glyph_index` | SJIS → glyph index |
| `800298d0` | `glyph_render_normal` | 1 bpp → 4 bpp + shadow, upload |
| `8002959c` | `glyph_render_condensed` | 8-px variant |
| `8002a7e4` | `glyph_cache_alloc` | allocates the 32 page descriptors |
| `8002a810` | `glyph_cache_clear_all` | clears descriptors |
| `8002a860` | `glyph_cache_page_setup` | (page, capacity, kind) |
| `8002a9b0` | `glyph_cache_page_free` | frees one page |
| `8002aa10` | `glyph_cache_page_reset` | count = 0 |
| `8002aa30` | `glyph_cache_find` | lookup, kind 0 |
| `8002ab64` | `glyph_cache_add` | insert, flush when full |
| `8002b9bc` | `glyph_cache_find_condensed` | lookup, kind 1 |
| `8002baec` | `glyph_cache_add_condensed` | insert, kind 1 |
| `8002ae00` | `text_draw` | main string renderer |
| `8002adc8` | `text_draw_grey` | wrapper with rgb 0x808080 |
| `8002b638` | `text_measure` | width/height of a string |
| `8002bd3c` | `text_draw_condensed` | 8-px renderer |
| `8002bd04` | `text_draw_condensed_grey` | wrapper |
| `8002c574` | `text_measure_condensed` | |
| `80029ef4` | `text_prim_full` | primitive pool check |
| `80029f70` | `text_icon` | inline icon / mini icon / big icon |
| `80029f40` | `text_icon_grey` | wrapper |
| `800288c8` | `text_draw_mini` | 8×7 half-width font |
| `80028898` | `text_draw_mini_grey` | wrapper |
| `80028cb4` | `text_draw_digits` | 6×11 ASCII/digits |
| `80028c84` | `text_draw_digits_grey` | wrapper |
| `80028554` | `text_draw_tiny` | 6×6 font |
| `80028524` | `text_draw_tiny_grey` | wrapper |
| `80029010` | `text_draw_bigdigits` | 16×21 digits |
| `8002a37c` | `sjis_to_mini` | SJIS → mini-font bytes |
| `80044684` | `bigname_load` | per-character `FONT\%4.4X.tim` loader (task; §7.11, in `ghidra/symbols`) |
| KAWSEG `801f03c8` | `vs_record_draw` | VS / result screen record strip (§7.11, in `ghidra/symbols`) |
| `80019ff0` | `dialog_setup` | message/Yes-No dialog |
| `8001a590` | `dialog_draw_cb` | dialog draw callback (no Ghidra function) |
| `8001a284` | `dialog_run` | modal loop |
| `8003c0b0` | `battle_card_panel_draw` | card info HUD |
| `800466d0` | `battle_banner_draw` | top banner with `P0/P1` |
| `80046a3c` | `battle_helpline_draw` | bottom help line |
| `80021078` | `msd_vm_create` | VM over an MSCD buffer |
| `80021118` | `msd_regs_alloc` | register file |
| `80021198` | `msd_vm_step` | runs until the next host command |
| `80021168` | `msd_vm_free` | |
| `KAWSEG::801ed178` | `tutorial_msd_load` | loads `B:\BETA.MSD` |
| `KAWSEG::801ed334` | `tutorial_msg_show` | `p` expansion + box |
| `KAWSEG::801ed2d8` | `tutorial_msg_draw_cb` | draw callback |
| `KAWSEG::801ed5a8` | `tutorial_script_step` | tutorial MSD host (§7.12) |
| `EVOSEG::801eb200` | `unit_msd_load` | loads `C:\EVENT\unit0%d.MSD`, makes the VM |
| `EVOSEG::801eacbc` | `fusion_shop_host` | Fusion Shop MSD host |
| `EVOSEG::801ebec0` | `unit_msg_slot_alloc` | free line of the page |
| `EVOSEG::801ebf4c` | `unit_msg_build` | line of the message page |
| `EVOSEG::801ec33c` | `unit_msg_reveal` | typewriter |
| `EVOSEG::801ec468` | `unit_msg_draw` | draws the page |
| `8004c320`, `8004c410`, `8004c828` | `syserr_screen_*` | system error screens |

Data:

| Address | Proposed name | Purpose |
|---|---|---|
| `80070a64` | `g_text_rgb_grey` | 80 80 80 |
| `80070a68` | `g_text_fullwidth_digits` | ０–９ SJIS |
| `80070b0c` | `g_kanji_sym_ranges` | {first, base} × 19 |
| `80070b58` | `g_kanji_row_ranges` | {first, base} per half-row |
| `80070e78` | `g_battle_banner_msgs` | 18 × char* |
| `80070ed4` | `g_battle_help_msgs` | char* table |
| `801d96fc` | `g_glyph_cache_pages` | 32 × 12 bytes |
| `801d9702/03` | `g_glyph_ink_l/r` | last glyph extents |
| `801d9704/06` | `g_systim_x/y` | |
| `801d9708/0c` | `g_text_w/h` | measure results |
| `801d9710/12` | `g_text_clut_x/y` | |
| `801d9714` | `g_text_prim` | |
| `801d9718/1c` | `g_kanji0/1` | font data |

## 9. Open questions

1. The OPENSEG code that expands `S`/`E` (slot): **behaviour observed at runtime, code not yet
   located.** The memory-card dialog copies the source string at OPENSEG `0x801E27F4` into the draw
   buffer `0x800E5638` each frame and replaces **every bare `S` and every bare `E`** with the slot
   digit (`SLOT` → `1LOT`, `START` → `1TART`, `GAME` → `GAM1` in a slot-1 run) — the bare-letter
   scan of the summary, so it corrupts English words and needs its own override. The consumer of
   the pointer table at `0x801f5440` is still not traced.
2. The SAISEG host command that shows MSD text in cities (op 0x0A–0x0E `cmd` id → text register):
   not traced. The tutorial path is known.
3. Where `DAT_801d7438` (text palettes) comes from, and whether the US palettes differ. The
   implementation sidesteps it: the ASCII path uses the JP CLUT base (`g_text_clut_x/y`) and the JP
   colours look right on screen.
4. **Answered: yes.** The US `BETA.MSD` writes the player name as `*p` (11 lines, e.g. `*p, now you
   know which Attack to choose`); §7.12.
5. **Answered: yes, both mechanisms work.** `overrides.json` entries with `"overlay": "OPENSEG"`
   replace overlay addresses (e.g. `dcb_movie_play`), and guest calls into main-EXE functions are
   routed through `function_table.c`, which the recompiler rewires to the override symbols —
   observed at runtime: the OPENSEG dialog reached `dcb_text_draw` (ASCII) and, with cheats off,
   `f_8002AE00` (SJIS).
6. The meaning of the FNT header byte 5 (`0x10` / `0x30`) and the MSD header word at +0x0C.
7. **Resolved:** the game is run headless (`DCB_HEADLESS=1 DCB_FAST=1`, pad script, save states and
   VRAM dumps), and the claims marked above were re-checked against runtime state (JP dialog,
   ASCII draw, framebuffer/VRAM layout).
