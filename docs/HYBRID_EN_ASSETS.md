# Hybrid English build: JP code + US assets

Research notes and plan for an English version of the port built from the **SLPS-03101 code** (Digimon
World: Digital Card Arena) plus **assets from the player's own SLUS-01328 dump** (Digimon Digital Card
Battle). No game data is in this repository. Nothing here is distributed except tools, C code, and
text we write ourselves.

Status: research only (September 2026). Every claim below comes from the extracted files
(`extracted/<serial>/fs/`) or from the Ghidra projects of both boot EXEs. Addresses are JP unless
marked US. Confidence is given as **high**, **medium**, or **low**.

Tools written for this study (none of them write into the repo):

| Tool | Purpose |
|---|---|
| `tools/disc/drv_unpack.py` | list / unpack / JSON manifest of any `*.DRV` |
| `tools/disc/drv_diff.py` | per-archive JP↔US entry diff (identical / same size / resized / one side only) |
| `tools/assets/dcb_containers.py` | PAK / ARC / TIS readers, `pak-diff`, `cdd-diff` |
| `tools/assets/tim2png.py` | TIM → PNG preview, `--scan` finds TIMs embedded in any container |

```sh
python3 tools/disc/drv_diff.py extracted/SLPS-03101/fs extracted/SLUS-01328/fs [--entries]
python3 tools/disc/drv_unpack.py unpack extracted/SLPS-03101/fs/B.DRV /tmp/dcb/jp/B
python3 tools/assets/dcb_containers.py cdd-diff /tmp/dcb/jp/B/CARD2.CDD /tmp/dcb/us/B/CARD2.CDD
python3 tools/assets/tim2png.py --scan /tmp/dcb/us/B/TITLE.ARC -o /tmp/dcb/png/title
```

---

## 1. Summary

* **The JP main game and the US game have the same content.** Both have 301 cards, 159 decks, 12
  cities, 142 opponent screens, and the same movie layout. The card stats are identical except for
  one byte on 3 cards. The extra JP content is a separate program: **PSX2.EXE, "Challenge D-1 Grand
  Prix 2001"** (チャレンジ ディーワングランプリ 2001). It is a tournament mode that `LoadExec`s out of the
  main game and uses W/X/Y/Z.DRV. It has no US counterpart. (high)
* All DRV archives share one format: a TOC tree, name-addressed, no compression. **The game opens
  files by name at run time** (`"B:\CARD2.CDD"` → `CdSearchFile("\B.DRV;1")` → TOC walk,
  `FUN_80015c24`). A rebuilt archive with entries of a different size therefore works, as long as the
  disc directory record for the DRV is updated. (high)
* **Text is the core problem.** JP draws Shift-JIS through an 11×11 1-bpp kanji font with a glyph
  cache. In that engine every single-byte ASCII character is a control code. US replaced the engine
  with an ASCII renderer that reads a proportional bitmap font baked into `SYSTEM.TIM`, uses a
  per-character width table, and escapes control codes with `*`. Both engines share the same control
  code set. Showing US text in JP code needs a **C override of about 3 functions**: draw, measure,
  and icon. (high)
* Most graphics are **drop-in**: same TIM count and same VRAM rectangles, only different pixels. This
  covers the E.DRV attack-name plates (567 PAKs), M_CARD/P_CARD level badges, OPENING, PARTNER,
  FRIEND, TRADE, BCARD, CBTL_SYS, 17 card TIMs, 19 F PAKs, and the movie. Some need conversion:
  TITLE (a different layout), SYSTEM.TIM (icons moved), the MATCH/WIN name plates (wider), and the
  TIS reorders. (high for the classification, medium for "no code patch needed" on each one)
* **Card DB and deck DB:** same record layout, but US widened the text fields (+8 bytes per card
  record, +6 bytes per deck). The best path is to keep the JP binary layout and **graft US strings
  into it**. Only 26 of 906 card effect-text lines need shortening. (high)
* Scripts (MSD, "MSCD" bytecode) use the same opcode set on both sides. US scripts are rewritten and
  re-flowed, and the whole files swap by path. (medium: the interpreter is not diffed yet)

## 2. DRV archive format (Q1)

Same format for A–G, P, and W–Z (verified by walking every archive on both discs):

```
TOC (one per directory; the root is at sector 0), array of 32-byte records, ended by type 0:
 +00 u8     type      0x80 directory, 0x01 file, 0x00 end
 +01 char3  ext       "PAK" "TIM" "ARC" "MSD" "CDD" "DEK" "FNT" "TIS" "BIN" "AXF" "TXT" "BAT" "SCC"
 +04 u32    sector    file start inside the DRV (x0x800); for a directory, its own TOC sector
 +08 u32    size      bytes (0 for directories)
 +0C u32    unix time
 +10 char16 name      ASCII (one US record is Shift-JIS, see below)
```

There is no compression at the DRV level. Entries are sector aligned and zero padded. `P.DRV`
(overlays) is the same format with `BIN` entries, so `tools/disc/pdrv_segments.py` is a special case.

Oddities:

* US `B.DRV` has a stale record, `コピー ～ CARD2.CDD` ("Copy of CARD2.CDD"), that points past the end
  of the archive. It is a Windows copy left in by the build tool, and the game never opens it.
  `drv_unpack.py` warns and skips it.
* US archives contain 11 `VSSVER.SCC` (Visual SourceSafe) junk entries. Ignore them.
* The runtime loader (`FUN_8001b48c` task → `FUN_80015c24` open → `FUN_800159fc` TOC match) parses
  `X:\DIR\NAME.EXT`, takes the DRV LBA from the ISO directory, then walks the TOC. The name is
  matched upper-case, and the extension is packed as a u24. The file size comes from the TOC, and the
  buffer is allocated to that size (`FUN_8001ae10`). So **bigger US files are fine**.

### 2.1 Inner containers (see `tools/assets/dcb_containers.py`)

| Format | Layout | Where |
|---|---|---|
| **PAK** | `u16 kind, u16 id, u32 size, data` chunks, ending with `FFFFFFFF`. kind 0 = header/names, 1/3/4 = model and animation, 2 = MSD script, 5 = image (TIM, or a TIS in `C:\AREAxx.PAK`), 6 = SEQ, 7 = VAB head, 8 = VAB body | A, C, E, F, G, W, X, Z |
| **ARC** | `u32 offset[n]` (offset[0] = 4n), entries back to back, all TIMs | B, C, Y |
| **TIS** | `"Tp", u16 n, u32 word_offset[n]` (×4), then TIMs | C, and inside C PAKs |
| **MSD** | `"MSCD", u32 3, u32 size, …` 16-bit-word bytecode with inline NUL-terminated strings | B, C, E, X, Y, F/C PAK chunks |
| **CDD** | card DB, §6 | B, Y |
| **DEK** | deck DB, `"20KD"` JP / `"30KD"` US, fixed records | B |
| **FNT** | `"TNF@"` 1-bpp kanji font, 22 bytes per glyph | B, Y (JP only) |

Within a PAK, JP and US store the same chunks in a different order (for example E `1129.PAK`). That
suggests lookup by (kind, id). This is not yet traced in code (medium).

## 3. JP↔US entry map (Q2)

`drv_diff.py` output, condensed (entries matched by path):

| DRV | JP / US entries | Identical | Changed | JP only | US only | Content |
|---|---|---|---|---|---|---|
| A | 104 / 105 | 103 | 1 (`BATTLE.PAK`, 1 TIM chunk) | – | SCC | BGM (SEQ+VAB PAKs), SE banks |
| B | 837 / 607 | 287 | 315 | 235: `FONT/*.TIM` ×232, `KANJI0/1.FNT`, `D1_LOAD.TIM` | `FONT.ARC`, SCC | cards, UI, fonts, databases |
| C | 53 / 57 | 10 | 18 | loose `EVENT/CITY*.MSD`, `OBJECT/AREA*.TIS`, `FACE00.TIM` | the same loose files under `DEBUG/`, SCC | city scripts and maps |
| E | 763 / 1319 | 177 | 568 | 18 PAKs (830–846, 994–997; all also in Z) | `TIM/` ×570, `SKILL/` ×3, `WAZA_ALL.BAT` | battle attack animations |
| F | 268 / 267 | 247 | 20 | `ERROR.TXT` | – | battle backgrounds and AI/effect scripts |
| G | 38 / 38 | 37 | – | `127.PAK` | SCC | misc models |
| P | 7 / 7 | 0 | 7 (code) | – | – | overlays (JP code must stay) |
| W, X, Y, Z | JP only | | | | | D-1 Grand Prix mode, §7 |
| MMM.DAT | identical | | | | | |
| DIGIMON.MOV | same size and layout | | video and part of the audio | | | §8 |

Classification of the changed entries, by what actually differs:

| Class | Count | Evidence |
|---|---|---|
| **Attack-name graphics** (E PAK, chunk kind 5, a 256×40 TIM at VRAM 640,0) | 567 PAKs, exactly one TIM chunk each | `pakcmp`: 567 differ only in kind-5 chunks. The US build left `E:\WAZA_ALL.BAT` in, 573 lines of `call wpack <pak> <tim>`, the PAK→name mapping the localizers used |
| **UI and card images**, same geometry | B: M_CARD 301/301 TIMs, P_CARD 19, PARTNER 21, OPENING 10, FRIEND 5, TRADE 4, BCARD 1, CBTL_SYS 16, 17 `CARD/LC*.TIM`, SYSTEM.TIM; F: 19 PAKs; A: BATTLE.PAK | `tim2png --scan`: identical (bpp, rect, CLUT) lists |
| **Images with a different layout** | TITLE.ARC (13 → 10 TIMs, new logos); MATCH/WIN ×284 (name-plate TIMs widened 136 → 192 px); TIS files reordered | §5 |
| **Text data** | CARD2.CDD, DECK2.DEK, BETA.MSD, C `EVENT/UNIT0x.MSD`, C `AREAxx.PAK` (kind-2 city scripts) | §4, §6 |
| **Fonts** | JP `KANJI0/1.FNT` and `FONT/<sjis>.TIM`; US `FONT.ARC` and new rows in `SYSTEM.TIM` | §4 |
| **Code** | P.DRV overlays, boot EXE | must stay JP |
| **Audio** | none (all 103 A PAKs identical) | |
| **3D models and animations** | none (every changed E/F/C PAK differs only in image/script chunks) | |

Findings about C.DRV: the loose `C:\EVENT\CITYxx.MSD` / `C:\OBJECT\AREAxx.TIS` (JP) and
`C:\DEBUG\*` (US) are duplicates. The game uses the copies packed into `C:\AREAxx.PAK` (kind 2 id
0xC8 = city script, whose size equals the loose MSD; kind 5 id 0xFA0 = a 40-entry TIS with the city
name plates).

## 4. Text (Q3)

### 4.1 Where text lives

| Location | JP | US |
|---|---|---|
| MSD scripts (tutorial `B:\BETA.MSD`, city events in `C:\AREAxx.PAK`, `C:\EVENT\UNIT0x.MSD`) | Shift-JIS, e.g. 128 strings / 7.5 KB in BETA, 1183 strings / 30 KB in CITY00 | ASCII, re-flowed (126 / 1159 strings) |
| Card DB `CARD2.CDD` | SJIS names, attack names, 4 effect lines | ASCII in the same slots (+2 bytes per line) |
| Deck DB `DECK2.DEK` | SJIS deck and owner names | ASCII |
| Overlays (P.DRV) `.rodata` | ~1600 SJIS strings, ~37 KB (OPENSEG 285, KAWSEG 313, SAISEG 291, SUBSEG 357, EVOSEG 184, ENDSEG 111, SUGSEG 52) | ~830 English strings, ~26 KB |
| Boot EXE | 427 SJIS strings, 6.6 KB | 214 English strings, 7.2 KB |
| Images | attack names, level badges, menus, title, city plates | English redraws |

Counts are from a string scanner (NUL-terminated SJIS/ASCII runs, 5+ bytes). They are approximate
(medium).

### 4.2 The JP text engine (Ghidra, SLPS_031.01)

| Address | Role |
|---|---|
| `FUN_8002d564` | loads `B:\KANJI0.FNT` → `DAT_801d9718`, `B:\KANJI1.FNT` → `DAT_801d971c` (+8-byte header) |
| `FUN_8002d130` | glyph address: SJIS 0x8140–0x84BE and anything outside 0x889F–0x9872 → KANJI0, else KANJI1 (JIS level 1); `font + index*0x16` |
| `FUN_8002d1c0` | SJIS → glyph index via range tables at `0x80070b0c` (non-kanji blocks) and `0x80070b58` (kanji rows) |
| `FUN_800298d0` | expands a 22-byte glyph (11 rows × 16 bits, 12 px wide) to 4-bpp, computes its ink extents (`DAT_801d9702/03`), and uploads it to a VRAM cache slot with `LoadImage` (3×12 halfwords) |
| `FUN_8002959c` | condensed (8 px) variant of the same, used through `FUN_8002baec` |
| `FUN_8002aa30` / `FUN_8002ab64` | glyph-cache lookup / insert (32 pages × 21 columns × 12 rows) |
| **`FUN_8002ae00`** | **draw string** → SPRT primitives |
| **`FUN_8002b638`** | **measure string** (width → `DAT_801d9708`, height → `DAT_801d970c`) |
| `FUN_80029f70` | inline icon (from `SYSTEM.TIM`, row base `v = 0x69`) |
| `FUN_80044684` | "big name" font: per-character `B:\FONT\%4.4X.tim` (32×32 TIMs named by SJIS code) into VRAM (0x2C0+, 0x1C0+row·0x20) |

The string language shared by both engines: bytes 0x81–0x98 start an SJIS pair. `\n` or the two
characters `\` `n` start a new line (+13 px). A space advances 6 px, and `z` toggles that to 12.
Digits map to full-width digits through `0x80070a68`. Two-character codes: `aN`/`bN`/`dN`/`eN`
draw an inline icon (12 px), `gN` draws a big icon (25 px), `cN` selects the CLUT/colour, `hN`
sets extra line spacing, `sN` turns proportional mode on or off, and `wN` sets letter spacing. Every
other ASCII byte is skipped, so **English text passed to the JP engine renders as nothing or
garbage**.

### 4.3 The US text engine (Ghidra, SLUS_013.28, functions created during this study)

| US address | Role | JP counterpart |
|---|---|---|
| **`FUN_80028d48`** | draw string: ASCII glyph `c-0x20` in a 16-per-row grid; u = col·6 + (width[c] >> 4), v = row·12 + 0x30, advance = width[c] & 0xF (proportional; fixed 6 when `*s0`); control codes are `*a` `*b` `*c` `*d` `*e` `*g` `*h` `*s` `*w` (same meaning as JP); `\n` and `\`+`n` break lines | `FUN_8002ae00` |
| **`FUN_800293fc`** | measure string (same rules) | `FUN_8002b638` |
| `0x8006df7c` | 256-byte width table (low nibble = advance, high nibble = u offset) | – |
| `FUN_80029a0c` | inline icon; row base `v = 0x7f` (JP 0x69), plus a new 5×5 icon mode 3 | `FUN_80029f70` |
| `FUN_80029efc` | legacy JP-style draw kept in the US EXE: SJIS pairs draw a fixed placeholder cell | – |
| `FUN_80041ca8` | big-name font from `B:\FONT.ARC` (64 TIMs of 16×32, indexed by `c - 0x20`, max 12 characters) | `FUN_80044684` |

The US glyph lookup has no counterpart: `FUN_8002d130` has no match above noise in the US EXE
(13/36 opcodes), and US ships no `KANJI*.FNT`. The US game cannot draw Japanese at all.

**Fonts:** the US small font is **inside `B:\SYSTEM.TIM`**. It is the same TIM geometry as JP (4 bpp,
VRAM 960,256, 256×256, CLUT 960,496). US replaced the JP hiragana and katakana rows (y ≈ 40–115)
with a larger ASCII set and moved the icon block down (v 0x69 → 0x7f). The big font is `FONT.ARC`
(US) versus 232 per-character TIMs (JP).

### 4.4 What the override must do

C overrides in `src/game/overrides/`, registered in `config/SLPS-03101/overrides.json`:

1. `FUN_8002ae00` / `FUN_8002b638` → ports of US `FUN_80028d48` / `FUN_800293fc`. Keep the JP
   argument order (JP: `x, y, clut, prop, rgb*, ot, str`) and the JP result globals
   (`DAT_801d9708/970c`), and read US glyph metrics from a width table imported from the player's
   US EXE. Recommended: handle both encodings, so SJIS pairs still go to the original JP path
   (call through to `f_8002ae00`, which is kept). Untranslated strings then still render, which
   makes the project incremental.
2. The escape character: US data writes control codes as `*e3`, JP data as `e3`. The ASCII-aware
   override must require `*` for codes. Converted JP strings (MSD, overlay text) must get `*`
   inserted. The US assets already have it.
3. `FUN_80029f70` (icons): use row base `0x7f` when the US `SYSTEM.TIM` is loaded (or re-author
   `SYSTEM.TIM`, §5).
4. `FUN_8002baec` family (condensed text; callers `FUN_8002bd3c`, `FUN_8002c574`) and
   `FUN_80044684` (big-name font) need the same treatment. The candidate US counterparts are
   `FUN_80028588` and `FUN_80041ca8`.
5. Line-width assumptions in callers (window widths, wrapping) are already solved by US data: the
   US strings contain their own line breaks.

Confidence: high for the mechanism, medium that items 1–4 are the complete list. Grep for more
`FUN_8002d130` / `FUN_8002ab64` callers before building.

## 5. Graphics with text (Q4)

| Asset | JP vs US | Verdict |
|---|---|---|
| E `NNNN.PAK` attack names (chunk kind 5, id varies, 256×40 4 bpp at VRAM 640,0) | 567 PAKs differ only in that chunk; US `E:\TIM\NNNN.TIM` (570 files) are loose copies | **drop-in**: whole-file swap, or graft only the chunk |
| B `M_CARD.ARC` (301 mini cards with level badge 完 → U), `P_CARD.ARC` (Ⅲ → R) | same geometry | **drop-in** |
| B `OPENING`, `PARTNER`, `FRIEND`, `TRADE`, `BCARD` ARCs, 17 `CARD/LCnnn.TIM`, F 19 PAKs, A `BATTLE.PAK` | same geometry, pixels differ | **drop-in** (spot-check in game) |
| B `CBTL_SYS.ARC` (battle UI, 27 TIMs, 16 changed) | same image rects; one CLUT is uploaded as 32×1 instead of 16×2 (the file grew 14 KB): TIM #17 @0x11240, the phase banner (JP 準備 / 進化 / 戦闘, US Prep / Digi-volve / Battle, 44×72 4 bpp at 948,304), CLUT at (816,497). The 32 US entries are the JP two rows back to back, colour for colour (two row-0 entries lose the semi-transparency bit), and the same bytes as the US #18's 16×2 palette at the same corner | **done** by `swap_us_images.py`: `reshaped()` pairs a US TIM whose palette is the JP one in another w×h (same corner and entry count, same RGB per entry, `is_palette_reshape`) as the JP-shaped TIM, so the US image goes in and the US palette is uploaded 16×2 as the JP game expects. The JP draw uses row 497 (CLUT 0x7C73) for the banner and row 498 (0x7CB3) for its semi-transparent zoom trail and the #18 frame; both rows keep their colours. Checked headless: Prep / Digi-volve for the opponent, Prep for the player |
| B `BG.ARC` | 9 non-TIM entries (probably compressed images), same count | unknown encoding; take-us and test |
| B `MATCH/nnn.ARC`, `WIN/nnn.ARC` (142 each) | same count and order; the two opponent-name plates grow from 34 to 48 VRAM units (136 → 192 px) | **drop-in if** the sprite width comes from the TIM; otherwise patch one width constant (**check `KAWSEG`**). Checked for MATCH: the TIMs at (464, 184/202) are the record strip (JP 戦 勝 敗, US blank), and the opponent's name picture is the last TIM (704, 480; JP 128 px, US 160–256 px) whose width *is* taken from the TIM. The port grafts only that TIM (`tools/text/bigfont.py`, [text-engine §7.11](re/text-engine.md#711-vs-screen-big-names)); WIN has no name picture. **Done** for the rest by `swap_us_images.py` (the archives read from the disc like the TIS files, paired per archive with `PARTIAL`): the 1st / 2nd turn cards (40×48 at 384/394,112) and labels (48×32 at 424/436,0), the player and opponent portraits (128×112 8-bit at 320,0 / 320,112: the card in hand has the US back), the WIN / LOSS banners (160×92 at 464,0 / 464,92). The record strip (464,184): the US one, blank, is cut to the JP 136 px (`NARROW`: the first 126 columns and the right 10; the result is the JP strip without the kanji, checked on the disc data) and the override `dcb_vs_record_draw` (KAWSEG 801F03C8, `src/game/overrides/vs_record.cpp`) draws Btl. / W / L where the kanji were; one picture in all 284 archives. The name pictures stay out of the swap (`KEEP_JP_AT`: bigfont.py grafts them, their palette is the big font's). The DECK NAME strip (464,202) is English on both discs. |
| B `TITLE.ARC` | 13 → 10 TIMs, new logo arrangement (デジモンワールド / デジタルカードアリーナ → DIGIMON / DIGITAL CARD BATTLE) | **done**: US logos re-packed into the JP slots. The US copyright is 256 wide where JP has 176: the US game grows the same VRAM slot in place ((512,168), 44 → 64 units, free on this screen), so the manifest gives it `"slot_w":256` and `sprites.txt` draws it 1:1; the subtitle is drawn smaller through `sprites.txt` (the title draws every image with OPENSEG `title_draw_sprite` 801ED8D0, a plain SPRT, which cannot scale; the packet pool is fixed 28-byte slots, so the resize happens in the GPU rather than by rewriting the packet) |
| B `SYSTEM.TIM` | same geometry; the kana rows became the ASCII font, and icons moved +22 px | **conversion**: use US `SYSTEM.TIM` and patch the icon base (§4.4), or composite the US font rows into free space in the JP sheet |
| C `AREAxx.PAK` (script + TIS: the city backgrounds plus a copy of the `WORLD.TIS` menu art) | same TIMs (39; 54 in AREA11), reordered; the same 7 redrawn in every city: city-name plates (192×120 sheet at 832,0), sub-menu and CITY MENU buttons (64×144 at 792,0 and 116×144 at 736,0; the two 16×2 palettes are the highlight), area signs and destinations (96×60, 116×72, 88×176), HELP MENU plate (88×80 at 808,0) | **done** by `swap_us_images.py`, paired by VRAM rect, one manifest entry per image for all 12 cities (the replacer keys by content). The HELP MENU plate (also shown on the world map) is recomposed (`COMPOSE`): JP "+ 移動 / ○ 入る / ✕ メニュー", US "+ Move / ✕ Enter / △ Menu", and this build keeps the JP controls, so the tool takes the US plate, moves its ✕ icon (11×11 at 13,26) to the Menu row (13,41) and puts the JP ○ icon on the Enter row; the ○ colours the US palette lacks (red 0x35DC, 0x20C5) take the two slots only the US △ used (`compose_image`), giving "+ Move / ○ Enter / ✕ Menu". The sub-menu labels are pre-warped (`JP_DRAW_SCALE`): the JP SAISEG draws each label cell as 68×16 texels on 64 pixels (the US one 64×16, 1:1), so texel columns 16, 33 and 50 never show; the thin US strokes broke there (the highlight looked partial), so each US column is moved to the texel the JP draw shows (the 3 rightmost background columns drop). `area_image_chunk()` gives the same graft as a kind-5 chunk for a PAK writer |
| C `OBJECT/WORLD.TIS`, `DECK.TIS` | same TIM set, reordered | **done** the same way (`DECK.TIS`: Card Menu / Deck Select / Deck Edit headers and the card icon sheets with the US level badges) |
| C `OBJECT/UNIT.TIS` | same 23 TIMs, reordered (13 redrawn: fusion/evolution captions and card faces) | **done** the same way |
| B `FONT/*.TIM` vs `FONT.ARC` | different scheme | **code override** (§4.4 item 4) |

## 6. Card and deck data (Q5)

`B:\CARD2.CDD`, loaded by JP `FUN_800481d8` / US `FUN_800457fc`:

```
+0  char[4] "ADCD" (JP) / "0ACD" (US)
+4  u16 n_digimon = 191, u8 n_item = 102, u8 n_option = 8        (301 cards, same on both)
+8  digimon[191] stride JP 0x134 / US 0x13C
    item[102]    stride JP 0x0DA / US 0x0E2
    option[8]    stride JP 0x068 / US 0x070
    (bytes 0-1 of each record are overwritten with the card number at load)
digimon: +03 name[21] · +18 stats · +26/+42/+5E attack names[22] with 6 bytes of stats between ·
         +E4 effect code · +E7 effect text: 4 lines × 19 bytes (JP) / 21 bytes (US)
item:    +03 name[21] · … · +8D effect text 4 × 19 / 21
option:  +03 name[21] · … · +1B effect text 4 × 19 / 21
```

`dcb_containers.py cdd-diff` compares every non-text byte of all 301 records. There are **3
differences**, all at digimon `+0x8E`: cards 5, 13, and 128 are JP 0x00/0x05/0x00 and US 0xFF. The
US field is probably a later balance or bug fix (the US build is dated 2001-05, JP 2000-11). All
stats, IDs, and effect codes are otherwise identical.

**JP data + US names/descriptions is straightforward.** Copy the US name and attack names into
the same slots (all fit). Re-slot the four effect lines from 21-byte to 19-byte slots: 880 of 906
fit as is, and 26 lines (max 20 characters) need hand-shortening or a slightly condensed wording.
Convert `*` control codes (US) as needed by the chosen renderer. Keep or adopt the 3 US `0xFF` bytes
(a design choice to decide later).

`B:\DECK2.DEK` follows the same pattern: 159 fixed records, JP stride 104 (loader at `0x80044150`,
`s0 * 104`), US 110. The card-ID lists are identical in the records sampled, and the name fields are
wider in the US file. Graft the names the same way (medium; not diffed field by field yet).

## 7. JP-only content (Q6)

`SLPS_031.01 FUN_80030300` shows `B:\D1_LOAD.TIM` ("チャレンジ ディーワングランプリ 2001 / Now Loading"),
shuts the game down, and calls `LoadExec("cdrom:\PSX2.EXE;1")`. PSX2.EXE (288 KB, a separate PS-X
EXE) is a **D-1 Grand Prix 2001 tournament game**. Its path strings are `Y:\CARD2.CDD`, `Y:\sugseg.bin`,
`W:\%03d.PAK`, `B:\SYSTEM.TIM`, `B:\KANJI0/1.FNT`, `A:\SE%d.PAK`, `A:\BGM\…`, `E:\SKILL\…`, and
`F:\bg%d.pak`. It shares the main game's B, A, and E drives, and Y carries its own copies of the
font and system files.

| DRV | Content | Overlap | English needs |
|---|---|---|---|
| X (205) | UI PAKs: TITLE, START, CHOOSE, MAKE (deck), MATCH_2/3/F, TYUUI (warning), BANDAI, DMP; `VJUMP/NAME/NAMEnnn.TIM` ×136 (256×40 name plates); BGM ×51; `SKILL0.MSD` | 5 files = A | new translation and redraw of all UI images and 136 name plates |
| Y (11) | its own SYSTEM.TIM/ARC, `CARD.CDD` + `CARD2.CDD` (older 49/52 KB format), KANJI fonts, SUGSEG.BIN, SKILL998/999.MSD (= US E) | 4 files elsewhere | card DB: map to main-game card IDs and graft US names (new format to decode) |
| Z (765) | attack PAKs | 658 byte-identical to JP E | reuse the US E name graphics (same chunk graft); about 107 need a check or new plates |
| W (197) | backgrounds (`W:\%03d.PAK`) | 5 = F | probably no text |
| PSX2.EXE | 188 SJIS strings, ~1.1 KB | – | new translation, plus the same text-engine override (it carries its own copy of the JP engine) |

This is **new translation work** (no US source exists), plus recompiling PSX2.EXE as a second program
(the README's open `LoadExec` item). Do it last.

## 8. The movie (Q7)

`DIGIMON.MOV.raw2352` has 34,072 sectors on both discs, **every XA subheader is identical**, and it
holds 4,893 MDEC frames, 320×160 at 15 fps (`ffmpeg -f psxstr`). There are three video segments,
starting at sectors 0, 14872, and 33168:

* **Opening** (0–14871, ~2:12): the same footage. The title logo differs (JP デジモンワールド デジタル
  カードアリーナ vs US DIGIMON DIGITAL CARD BATTLE), and one card close-up differs. Most video sectors
  differ because the whole stream was re-encoded. **All audio sectors in this segment differ** (a
  different or re-mixed song track; not verified by ear).
* **Credits** (14872–): the same footage with the credits text in Japanese vs English. The audio is
  identical.

So the movie differs in the logo, the credits text, and the opening audio. The layout is identical,
so **the whole US file is a drop-in swap** (high).

## 9. How it plugs into the port

### 9.1 Data layer: a DRV rebuild in `ExtractedDisc`

The game resolves files by name through the ISO directory and the DRV TOC, so the cleanest hook is
**below the game**, with no guest code patched:

1. **Import step (host tool, run by the player on their own dumps):**
   `tools/hybrid/build_en.py --jp extracted/SLPS-03101 --us extracted/SLUS-01328 --out extracted/SLPS-03101-en`.
   It unpacks both discs, applies a **recipe** (§9.3) of per-entry rules (`take-us`,
   `graft-pak-chunk kind=5`, `cdd-graft`, `dek-graft`, `tis-reorder`, `text-patch`), and writes
   rebuilt `B.DRV`, `C.DRV`, `E.DRV`, and so on, plus `DIGIMON.MOV.raw2352`.
2. **`ExtractedDisc`** already rebuilds sectors from `layout.txt`. Extend it so that a file whose
   size changed gets **new LBAs appended after the last sector**, and patch its ISO9660 directory
   record (LBA and size, both-endian) in the in-memory copy of `iso_meta.bin`. libcd's
   `CdSearchFile` (recompiled game code) then finds the new extent. Same-size replacements (P.DRV
   text patches, the movie) keep their LBAs. The alternative, re-laying out the whole disc, also
   works but changes every LBA. Appending is safer if any code seeks by absolute LBA.
3. Nothing from either disc is committed or distributed. The recipe, the text patches we write
   (translations of JP-only strings, the 26 shortened card lines), and the tools are ours to ship.

Rejected alternative: a C override of the DRV open (`FUN_80015c24`) that serves host files. The
game reads the data afterwards via CD-ROM commands and sector callbacks (`FUN_80016080`), so the
override would also have to fake the CD transfer. That is more code for the same result.

### 9.2 Code layer: C overrides

* Text engine: §4.4 (draw, measure, icon, condensed, big font). This is the one mandatory code
  change.
* Strings inside code (boot EXE and P.DRV overlays): the recompiled code has string addresses
  baked into its `lui/addiu` constants, so pointers cannot be redirected in data. Options:
  * **In-place replacement** of the bytes in the JP P.DRV/EXE image. This works for most strings,
    because SJIS takes 2 bytes per character (for example 練習レンタル is 12 bytes, "Practice" is
    8). Overlay dispatch checks only the first 16 code bytes of each function
    (`find_overlay_function`), so `.rodata` edits are safe. P.DRV keeps its size and LBAs.
  * For strings that do not fit: a C override of the function that uses them, or a recompiler
    feature (a `config/strings.json` of address → replacement that the generator emits as a lookup).
  * US counterparts can be paired semi-automatically: overlays are the same source code, so match
    functions JP↔US (fuzzy opcode matching found the US text functions this way) and pair their
    string references.
* Width constants for the wider name plates (MATCH/WIN), if §5 shows they are hard-coded.

### 9.3 Recipe sketch

```yaml
# recipes/en.yaml — rules only, no data
- {drv: E, match: "*.PAK", rule: take-us-if-only-image-chunks-differ}   # 567 PAKs; model chunks verified equal
- {drv: B, match: [M_CARD.ARC, P_CARD.ARC, OPENING.ARC, PARTNER.ARC, FRIEND.ARC, TRADE.ARC, BCARD.ARC, CBTL_SYS.ARC, BG.ARC, "CARD/LC*.TIM"], rule: take-us}
- {drv: B, match: "MATCH/*.ARC", rule: take-us}          # + width check
- {drv: B, match: "WIN/*.ARC",   rule: take-us}
- {drv: B, match: SYSTEM.TIM, rule: take-us}             # + icon-base override
- {drv: B, match: TITLE.ARC, rule: custom-title}
- {drv: B, match: CARD2.CDD, rule: cdd-graft, fixes: patches/cdd_lines.yaml}
- {drv: B, match: DECK2.DEK, rule: dek-graft}
- {drv: B, match: BETA.MSD, rule: take-us}
- {drv: C, match: ["AREA*.PAK", "EVENT/UNIT*.MSD"], rule: take-us}
- {drv: F, match: "*.PAK", rule: take-us}                # 19 image-only changes
- {drv: A, match: BATTLE.PAK, rule: take-us}
- {file: DIGIMON.MOV.raw2352, rule: take-us}
- {drv: P, rule: text-patch, patches: patches/overlay_strings.yaml}
- {file: SLPS_031.01, rule: text-patch, patches: patches/exe_strings.yaml}
```

## 10. Feasibility per asset class

| Asset class | Rating | Notes |
|---|---|---|
| Attack-name graphics (E) | **drop-in swap** | whole PAK or kind-5 chunk; 567 files |
| Mini cards, badges, card art with text, battle UI sheets, F backgrounds | **drop-in swap** | same geometry |
| Opponent intro/win screens (MATCH/WIN) | drop-in, **maybe a 1-constant patch** | name plates are wider |
| City scripts + city name plates (C AREAxx.PAK) | **drop-in swap** (medium) | TIS order question |
| Tutorial and event scripts (BETA.MSD, UNIT0x.MSD) | **done** (US script + the JP button tests; host overrides) | [text-engine.md §7.11](re/text-engine.md#711-tutorial-and-fusion-shop-scripts-bbetamsd-ceventunit0nmsd) |
| Movie | **drop-in swap** | identical layout |
| SYSTEM.TIM (small font + icons) | **needs conversion or code override** | icon row moved |
| Title screen | **needs conversion or code override** | different layout |
| Card DB, deck DB | **needs conversion** (graft) | 26 effect lines to shorten |
| Big-name font (FONT.ARC) | **needs code override** | |
| Text renderer | **needs code override** (mandatory) | ~3–5 functions |
| Strings in EXE/overlays (~2000) | **conversion** (in-place) + a few overrides | US text exists for all |
| D-1 Grand Prix (PSX2.EXE, W/X/Y/Z) | **needs new translation** + second-program recompile | ~190 strings, ~150 images, card DB |

## 11. Staged plan

1. **Tooling foundations (done here):** unpack, diff, container readers, TIM preview.
2. **Disc rebuild with appended extents** in `ExtractedDisc` + `tools/hybrid/build_en.py` with the
   `take-us` rule. First visible win, **with no code change**: E attack names, mini cards, level
   badges, battle UI, opponent screens, and the English movie. Verifies the name-based loading
   end to end.
3. **Text renderer override** (draw, measure, icon, with a SJIS fall-through) + US `SYSTEM.TIM` +
   width table imported from the player's US EXE. Unlocks the English tutorial, city scripts, and
   unit scripts via `take-us`.
4. **Card and deck DB graft** (`cdd-graft`, `dek-graft`, 26 hand-shortened lines as our own patch
   file).
5. **Code strings:** a JP↔US overlay function pairing tool, then in-place patches for EXE and P.DRV
   strings, plus overrides where strings do not fit. This is the largest manual QA step.
6. **Title screen, big-name font, leftovers** (TITLE.ARC, FONT.ARC override, UNIT.TIS, any
   hard-coded widths).
7. **D-1 Grand Prix:** recompile PSX2.EXE, reuse the text override, graft Z attack names from US E,
   translate the ~190 strings and ~150 images (new work, can be community-sourced), and map the Y
   card DB.

## 12. Open questions

* Does KAWSEG draw the MATCH/WIN name plates with a hard-coded width (136 px)?
* Are TIS entries used by index or by VRAM rect? This decides take-us vs reorder for AREA, WORLD,
  and DECK. How does `UNIT.TIS` (19 vs 20 entries) map?
* Is the MSD interpreter identical between the JP and US overlays (same opcode handler table)? The
  opcode streams look the same (0x05/0x07/0x08/0x09/0x0A/0x0B/0x0C…), but the US scripts are
  re-flowed and jump offsets differ. That is fine if all jumps are relative to the script.
* Do any JP code paths draw kana directly from the `SYSTEM.TIM` kana rows (not via KANJI.FNT)? If
  yes, the US `SYSTEM.TIM` breaks them, and a composited sheet is needed.
* The complete caller set of the glyph path (`FUN_8002ab64`, `FUN_8002aa30`, `FUN_8002d130`).
* The meaning of digimon `+0x8E` (0xFF in US for cards 5, 13, and 128): keep JP or adopt US?
* Opening audio: a different song, or only a re-mix? (Listen to both.)
* Y `CARD.CDD` / `CARD2.CDD` format for D-1 GP, and the Z attack PAKs that are not in E.
* Does anything seek `DIGIMON.MOV` or the DRVs by absolute LBA? (The appended-extent design avoids
  moving existing files, so this only matters for rebuilt ones.)
