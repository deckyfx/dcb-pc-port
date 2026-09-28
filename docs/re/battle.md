# Battle data (SLPS-03101)

The players' stats during a card battle, found by translating the US version's GameShark codes
(SLUS-01328, by Code Master) to the JP build. Used by the trainer's Battle tab and its hotkeys
(F10 applies the P1 actions that are on, F11 the P2 ones, F12 puts the stats back;
`src/game/overrides/battle.cpp`).

## Where it is

Each player's battle data is reached through a pointer table at **`801DAF40`** (one word per
player: 0 = you, 1 = the opponent); the US table is at `801D8348`. The same struct holds the
player name at `+0x1CA` ([text-engine.md](text-engine.md)).

| Field | JP offset | US offset | US code address (P1) |
|---|---|---|---|
| HP | `+0x118` | `+0x11C` | `800B3E84` |
| Circle / triangle / cross attack | `+0x11A` / `+0x11C` / `+0x11E` | `+0x11E` / `+0x120` / `+0x122` | `800B3E86` / `88` / `8A` |
| DP | `+0x120` | `+0x124` | `800B3E8C` |
| Circle / triangle / cross attack, current battle | `+0x158` / `+0x15A` / `+0x15C` | `+0x15C` / `+0x15E` / `+0x160` | `800B3EC4` / `C6` / `C8` |

The US codes use fixed addresses (P1 struct at `800B3D68`, P2 `0x1BB10` later); the JP build
has the same layout 4 bytes shorter before these fields.

## How it was found

1. The US DP codes include `8003E56E 2400`, a code patch: it turns the instruction at
   `8003E56C`, `sh v0, 292(v1)`, into `addiu zero, zero, 0x124` (a no-op). Disassembled, that
   store is in `8003E4F0`: for both players it calls six stat helpers, then stores
   `DP = 8004110C(player)` at `table[player] + 0x124`, with the table at `801D8348`.
2. The JP build has the same function at **`80040EF4`** (`battle_stats_update`): same loop,
   table at `801DAF40`, DP from **`80043B00`** (`battle_dp_calc`) stored with
   `sh v0, 288(v1)` at `80040F70`.
3. Offsets: counting every halfword load/store offset in both executables, the US
   `+0x11C..+0x126` and `+0x15C..+0x162` families appear in JP 4 bytes lower with matching
   counts (e.g. HP: US `284` ×32, JP `280` ×34).

The current-battle attacks (`+0x158..`) rest on the counts only; the in-game test is the
confirmation.

## Limits

The game caps HP and the attacks at 9990 and DP at 90 (the US codes write 9999 and 99); the
hotkeys use the game's caps.

## DP

The game recalculates DP every update, so a written DP would be undone. The US codes no-op the
store; this port overrides `battle_dp_calc` instead (`dcb_dp_calc`): after a hotkey, it returns
the set value until the battle ends (the calculation stops running for ~10 s, or the player's
data pointer changes) or F12 resets.

## Reset

F12 writes back each field's value from before the first change in this battle (kept per player
while its data pointer stays the same) and releases the DP locks.

## Deck shuffle

The deck is 30 card bytes at `+0x179`; drawn cards are at the front, the `n` undrawn ones at the
end (`n = deck_cards_left(player)`, `80042C24`). `deck_shuffle(player)` (`80043E24`) runs `+0x116`
passes (u16, "shuffles pending") of swapping each undrawn card with a random undrawn one
(`rand() % n`), then clears `+0x116`. Battle start calls it (KAWSEG `801F5518`), and so do card
effects that shuffle, e.g. Reserve Seven (card 291): they set `+0x116` to 300 and call it (KAWSEG
`801EBC50` / `801EBC80`). `deck_shuffle_2` (`80043F78`) is the same code over a second 30-byte
array at `+0x197`, not worked out.

The trainer's Battle tab has "deck in order (no shuffle)" for P1 and P2: while one is on,
`dcb_deck_shuffle` only clears the pending count, so that player draws in deck order (the order
of the deck as built). Boss A's "move the partner to the bottom, no shuffle" is presumably the
game doing the same for its own deck; its code was not traced.
