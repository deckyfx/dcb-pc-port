# Trainer (cheats and memory search)

[Home](Home.md)

`F4` (`[hotkeys] trainer`), or Trainer in the `F1` menu, opens a panel over the game, which stays
paused while it is open; `F4` or `Esc` closes it, `Tab` switches between its pages:

- **General:** things outside a card battle.
  - Presets: cheats built into the port for this game (all cards ×4, all 127 Digi parts). They
    cannot be edited; their on/off state is saved in the cheat file as `!preset <name> on|off`.
  - "Outside battle" toggles (saved as `!toggle` lines, as before when they were on the Battle
    page): every Fusion Shop fusion mutates, mutations give a Digi-Jewel, and all Digimentals
    (their flags, handed out at the next city Menu to the partners you have). Izzy's Mutation
    Detector does not block mutations: the keeper only warns, and "yes" still fuses.
  - Partners 1-3: `Left`/`Right` pick a Digimon (Veemon, Hawkmon, Armadillomon, Gatomon,
    Patamon, Wormmon, or empty) for that slot, `Enter`/`Space` writes it; the row shows what the
    game has now. It is written once (not kept in the cheat file): save in game to keep it. A new
    partner starts at level 1 with no Digi parts or Digimentals, like one the game gives you (the
    next city Menu hands out its Digimentals whose story flags are set); it takes the place of the
    old one in your decks. Picking a partner that is already in another slot swaps the two.
    Refused, with the reason on the status line: no save loaded, an empty partner 1 or a gap
    before a filled slot, removing a partner still in a deck, or a partner whose card is in a deck
    as a plain card (only possible with the all-cards preset). Change partners on the city map
    rather than during a battle or on the Partner screen.
  See [docs/re/save-data.md](../re/save-data.md).
- **Battle:** during a card battle, `F10` (`[hotkeys] battle_p1`) applies the P1 lines that are
  on (HP, circle/triangle/cross attack, DP, each with its value), `F11` (`battle_p2`) the P2
  lines, and `F12` (`battle_reset`) puts every stat they changed back. Values are multiples of 10
  up to the game's caps (9990, DP 90): Left/Right step by 10, PgUp/PgDn by 1000, or type a
  number and press Enter. Two more lines, "P1 / P2 deck in order (no shuffle)", work on their own
  while ticked: that player's deck is never shuffled, so cards are drawn in deck order. Saved in
  the cheat file as `!battle` lines. See [docs/re/battle.md](../re/battle.md).
- **Custom:** your own codes from the cheat file below, and what you freeze on the Search page.
- **Search:** memory search, below.

The `F1` menu's Hotkeys page lists every hotkey and what it does.

## Cheat file

Cheats live in `cheats/<serial>.txt` (e.g. `cheats/SLPS-03101.txt`), looked up in the current
directory, then next to the executable (`DCB_CHEATS=<file>` overrides); the folder is gitignored
and [`docs/cheats.example.txt`](../cheats.example.txt) is a template. The format is a name in
brackets with `on`/`off`, then PS1 GameShark / Action Replay lines:

```
# comment
[Infinite money] on
800B1234 270F      ; 16-bit write
```

Supported code types:

| Type | Meaning |
|---|---|
| `80` / `30` | 16 / 8-bit write |
| `10` / `11`, `20` / `21` | 16 / 8-bit increment / decrement |
| `D0`–`D3`, `E0`–`E3` | 16 / 8-bit ==, !=, <, > conditions on the next line; consecutive conditions must all hold |
| `C0` | gate the rest of the cheat |
| `50` | serial repeater |

`C1`, `C2`, `D4`–`D6`, `1F` and any other type are rejected with a message and the cheat is never
half-applied. Enabled cheats are written once per frame at the frame boundary.
`DCB_TRACE_CHEATS=1` logs every frame that has at least one write: the writes, and how many of the
written bytes held a different value in RAM before the write (e.g. a value the game had changed
since the last frame).

## Pages

- *Cheats page*: `Up`/`Down` select, `Enter`/`Space` on/off, `Del` remove, `R` reload the file,
  `S` save it (toggles, removals and frozen values; your comments are kept).
- *Search page*: pick the value size (8/16/32-bit) and signed or unsigned with `Left`/`Right`, type a
  value (decimal, `-5`, `0x1F` or `$1F`), pick a filter (`= != > <` value, or `changed`,
  `unchanged`, `increased`, `decreased` since the last filter) and press `Enter`. Workflow: search
  the current amount, close the panel, let it change in game, reopen and filter again until a few
  addresses remain. On a result, `F` freezes it (adds an enabled cheat holding the typed value, or
  the current one if the field is empty; `S` on the Cheats page saves it) and `W` writes the
  typed value once. The first 500 results are listed; the count is always shown.
