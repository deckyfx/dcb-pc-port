# Battle data (SLPS-03101)

The players' stats during a card battle, found by translating the US version's GameShark codes
(SLUS-01328, by Code Master) to the JP build. Used by the battle hotkeys (F9-F12,
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

## DP

The game recalculates DP every update, so a written DP would be undone. The US codes no-op the
store; this port overrides `battle_dp_calc` instead (`dcb_dp_calc`): after a hotkey, it returns
the set value until the battle ends (the calculation stops running for ~10 s, or the player's
data pointer changes).
