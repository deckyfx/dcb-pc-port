# Game data and save (SLPS-03101)

Where the game keeps the player's progress, and which functions change it. Found with the write
watch (`./dcb.sh -W`, [../RE_WORKFLOW.md](../RE_WORKFLOW.md)) during real play, and checked in
the code. Confidence: **H** read in the code and seen at run time, **M** seen at run time, role
inferred.

## Layout

The save (one 16 KB memory-card file, `BISLPS-03101_A`) is loaded as a whole to RAM
`800DEFC4`, so **RAM = `800DEFC4` + save offset**. The first 0x200 bytes are the memory-card
header (title, icon). The game works on the data after it: the word at **`80070C2C`** holds the
pointer, **`game_data` = `800DF1C4`** (save `0x200`). Offsets below are from `game_data`; add
`0x200` for the save file, `800DF1C4` for RAM.

Per-player data repeats every **10040 bytes** (`0x2738`): player 0 is you (the second copy,
`game_data + 0x2738`, is the opponent's side in battle).

| Offset | Size | What | Written by | Conf. |
|---|---|---|---|---|
| `+0x24` | u32 | play time (frames) | `playtime_tick` (`80014E24`), every frame | H |
| `+0x3C` | 16 bytes | Digi parts owned, one bit per part (parts 0-126), RAM `800DF200`-`800DF20F` | `digipart_give` (`8004BE48`); `digipart_has` (`8004C010`) tests | H |
| `+0x2F9` | u8 | partner level | `partner_gain_exp` (KAWSEG `801F7600`) | M |
| `+0x2FA` | u16 | partner EXP | `partner_gain_exp` (KAWSEG `801F7600`) | M |
| `+0x818` | u16 × 32 | battle counters (capped at 999), role unknown (maybe wins per opponent), RAM `800DF9DC` | `battle_counters_add` (KAWSEG `801FCF78`) | M |
| `+0x1482` | u8 × 301 | card collection, one byte per card number, RAM `800E0646` | `collection_add_card` (`8004850C`) | H |
| `+0x272C` | u16 × 3 | last battle's reward cards (`0xFFFF` = none) | `battle_rewards_pick` (`80048D68`) | H |

### Card collection byte

| Bits | Meaning |
|---|---|
| 0-2 | copies owned; `collection_add_card` stops at 6 |
| 0x10 | full (set with the count at 6) |
| 0x20 | "new": set on the first copy of a never-seen card, cleared by `collection_clear_new` (`800484AC`) |
| 0x40 | seen before (no "new" marker on the next first copy) |
| 0x80 | obtained |

`collection_add_card(player, card, count)` refuses cards 172-190 (returns -3); those 19 match the
19 images of `P_CARD.ARC`, so they are probably the partner cards, which the game gives another
way.

Examples from two battles: Tentomon (97) `00 -> E2` (first two copies: obtained, seen, new, 2),
Palmon (98) `C1 -> C2`, card 30 `41 -> 42 -> C2`.

## Battle rewards

1. `battle_rewards_pick` (`80048D68`) writes the three reward card numbers at `+0x272C` when the
   battle is set up (and `collection_clear_new` clears the "new" flags first).
2. After the battle, `battle_rewards_grant` (`80049280`, **M**) calls `collection_add_card` for
   each; a Digi part won is set with `digipart_give` (part 15 in the recorded run: `+0x3D`
   `04 -> 84`), and KAWSEG `battle_counters_add` adds one to the battle counters at `+0x818`.
   (That array was first taken for the Digi parts: a cheat filling it gave nothing. The
   starting partners' table at `80071AD8`, `0A 0F 00`, gives the three starter parts 10, 15
   and 0.)
3. `partner_gain_exp` (KAWSEG `801F7600`) raises the partner's EXP one step per frame, then the
   level (Veemon 3 -> 4: EXP `0x11 -> 0x18`, level `3 -> 4`).

## Other writers seen (role not worked out)

`8004835C` (called by `collection_add_card` per copy; the busiest writer while a save loads),
`80036BEC` (small counters at `+0x36`/`+0x38` and `+0x276E`.. during battle), `80041650` (after
a battle: `+0x250E` +1, likely a win count), KAWSEG `801EF968` and `801ED064`.

## Trainer codes

`cheats/SLPS-03101.txt` (local): all cards ×4 (cards 172-190 left out) and all 127 Digi parts,
as repeat codes over the tables above. See [../cheats.example.txt](../cheats.example.txt).
