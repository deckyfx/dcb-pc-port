# Community fixes

[Home](Home.md)

Fixes published for this game on romhacking.net (made for the US disc image) are applied to
SLPS-03101 too. Download them yourself (they are their authors' work, never in this repo) and put
the `.xdelta` files in `assets/SLPS-03101/fixes/`; the English converter
([English Text](English-Text.md)) then applies them to the US reference data it takes English from
(card text, strings, scripts), and carries data changes in the overlays into the SLPS overlays
(`files/P/`). Supported and tested:

| Fix | What it does here |
|---|---|
| [Digi-Parts Fix](https://www.romhacking.net/hacks/8474/) by Gledson999 | Digi-Part 037 (Eat-up HP) no longer missable with the Veemon, Gatomon or Wormmon partner: the SLPS overlays have the same table and the same bug |
| [Effect Text Fix v2](https://www.romhacking.net/hacks/9360/) by jota_verso | effect text of Aquilamon, Dolphmon, AeroVeedramon, Sylphymon, Veedramon, Ankylomon and Special Digivolve matches what the cards do; Tentomon's attack reads Super Shocker |

The converter prints each fix it applied and every byte it carried over (`fix: ...`); remove a
file from `fixes/` and re-run it to drop that fix. Notes: [tools/text/fixes.py](../../tools/text/fixes.py).
