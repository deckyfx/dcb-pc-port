# English text

[Home](Home.md)

The game stays SLPS-03101 (Japanese code); English text is taken from the player's own US dump
(SLUS-01328) and drawn by a native override of the JP text renderer. Plain-ASCII strings are drawn
with the US font and widths, while Shift-JIS strings keep going through the JP renderer
(untranslated Japanese still shows).

## Building the English data

The English data is built from the player's own dumps (both [imported](Game-Data.md)) into
gitignored `assets/`:

```sh
python3 tools/text/en_text.py --jp extracted/SLPS-03101 --us extracted/SLUS-01328 --out assets/SLPS-03101
```

It writes, into `assets/SLPS-03101/`:

| Output | Contents |
|---|---|
| `en_font.bin` | US font rows + width table |
| `files/B/CARD2.CDD` | US card names, attack names, effect lines |
| `files/B/DECK2.DEK` | US deck / owner names |
| `files/C/AREAnn.PAK` | the 12 city PAKs with the US city script (the image chunk stays JP) |
| `files/P/<SEG>.BIN` | SLPS overlays with the data changes of [community fixes](Community-Fixes.md) |
| `text/source.tsv`, `text/en.tsv` | the text catalog: whole game strings (e.g. the load screen's messages) as JP templates and their English |
| `en_names.txt` | names too long for their JP slot, drawn in full by the renderer |
| `en_text_report.txt` | lines too long for the JP slots |

Nothing copyrighted is printed: counts, offsets, the overlong list, and a few diagnostics in hex
(the before/after bytes of each overlay change carried over from a community fix, and a handful of
JP/US byte comparisons, e.g. the card balance bytes kept JP). The script's header
([tools/text/en_text.py](../../tools/text/en_text.py)) documents each output in detail.

The files under `files/` are loose-file replacements picked up by the native file access (see
[Game Data](Game-Data.md#native-file-access)).

## Behaviour

- Without `en_font.bin` the game draws everything with the JP renderer; deleting `files/B/` brings
  the Japanese card and deck text back.
- The font lives in a private texture sheet in the GPU, not in the game's VRAM.
- The text catalog is listed under `config/SLPS-03101/text/` (ids and offsets; the text comes
  from the dumps, except the port's own English in `config/SLPS-03101/text/en*.tsv` for strings the
  US version lacks or words differently, which the converter uses before the dump text; see
  [tools/text/catalog.py](../../tools/text/catalog.py)). `DCB_LANG=<lang>` picks `text/<lang>.tsv` (default `en`).
- `DCB_TRACE_TEXT=1` logs the text-engine calls, draw and measure (`DCB_TRACE_TEXT=hex` adds each
  string's raw bytes and whether the catalog translates it), which helps find what is still Japanese.

## Character names

The English text uses the US names; `[text] names` in `settings.ini` picks which ones are shown:
`jp` (the default) swaps them for the Japanese names, `us` keeps the US ones.

| US | Shown with `names = jp` |
|---|---|
| Davis | Daisuke |
| Keely (Yolei) | Miyako |
| Cody | Iori |
| T.K. | Takeru |
| Kari | Hikari |
| Tai | Taichi |
| Matt | Yamato |
| Izzy | Koushiro |
| Joe | Jou |

Ken, Sora and Mimi are the same in both; the Digimon Emperor becomes the Digimon Kaiser.

The Digimon take their Japanese card names too (53 of the 191 differ): Veemon -> V-mon, ExVeemon ->
XV-mon, Gatomon -> Tailmon, Salamon -> Plotmon, Omnimon -> Omegamon, Myotismon -> Vamdemon,
Piedmon -> Piemon, Puppetmon -> Pinocchimon, Machinedramon -> Mugendramon, ... Variants keep the
Japanese suffix (R-Gatomon -> Tailmon R, J-Mojyamon -> JungleMojyamon); MasterTyrannomon keeps its
US name. Every name fits where the longest US one (HerculesKabuterimon, 19 letters) does, and stays
within the battle field's 20-letter card-name buffer (a longer one would keep the US name).

The list is `config/SLPS-03101/text/names-jp.tsv` (built
into the program): one `US<TAB>JP` pair per line, whole words only, so editing it changes the names
everywhere the text shows them: dialogue and speaker lines, menus, card lists, the battle
field's card names, Fusion Shop lines, deck owners. Lines that are typed
out get the swapped name before they start, and the player's own name is never changed. On the VS
screen a renamed opponent's name is drawn with the big font of the player's name (the US pictures
are artwork with letters the Japanese names would need missing, such as Y).

Notes: [docs/re/text-engine.md](../re/text-engine.md). Research and plan for the full English
build: [HYBRID_EN_ASSETS.md](../HYBRID_EN_ASSETS.md).
