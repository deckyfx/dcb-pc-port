# Reverse-engineering notes

One page per subsystem. Each page names the guest functions involved (with
Ghidra addresses), the resources they touch, and how the finding was verified
(coverage diff, load log, poke). New pages go here; the loop itself is
documented in [../RE_WORKFLOW.md](../RE_WORKFLOW.md).

## Conventions

- Addresses are guest addresses (`0x80058CFC`), with the overlay name for
  overlay code (`KAWSEG::801E2A6C`).
- Names live in `ghidra/symbols/<serial>.json` (reviewed, diffable text).
  Import into Ghidra with `ghidra/scripts/import_symbols.py`, export new names
  with `ghidra/scripts/export_symbols.py`.
- Every claim cites its evidence: a coverage diff, a load-log excerpt, or a
  poke (override address + observed effect).

## Pages

- [Game data and save](save-data.md) — save layout in RAM, card collection, Digi parts, partner
  slots, battle rewards, progression flags (city flags, partners, Digimentals, the Mutation
  Detector), the Fusion Shop roll, and the functions that write them.
- [Battle data](battle.md) — players' HP, attacks and DP in a card battle (translated from the
  US GameShark codes), and the battle hotkeys.
- [Battle Cafe](battle-cafe.md) — the city script VM's ops, the city host's cafe and battle
  commands, how a cafe lists its opponents and starts a battle, the face sheet, the bosses' flags.
- [Text engine](text-engine.md) — SJIS/ASCII encoding, the JP/US renderers, fonts and glyph
  cache, string sources, override plan and the state of the English-text port.
- [Card lists](card-lists.md) — the shared list cursor (L2/R2 page), which screens are card
  lists, what L1/R1 do there, and the port's Left/Right paging.
- [Name entry](name-entry.md) — the player / deck name and keyword entry screens (character
  grid, tab list) and the override that opens them on the letters page.
