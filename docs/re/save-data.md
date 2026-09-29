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
| `+0x14` | u16 | a progress count: city host `0x0B 0x12 n` adds n (the city scripts add 1 per flag at start-up; host `0x0A 0x13` clears it) | `city_event_host` (SAISEG `801E6618`) | L |
| `+0x24` | u32 | play time (frames) | `playtime_tick` (`80014E24`), every frame | H |
| `+0x28` | u32 | flag word; bit `0x400` set by city host `0x0A 0x12`, bit 9 cleared by `game_data_init` | `city_event_host` | L |
| `+0x2C` | u32 | Fusion Shop flags, bits 0-9 = shop script registers r20-r29 ([below](#fusion-shop-flags-0x2c)) | `city_fusion_flags` (SAISEG `801E3628`), `fusion_flags_save` (EVOSEG `801EB914`) | H |
| `+0x3C` | 16 bytes | Digi parts owned, one bit per part (parts 0-126), RAM `800DF200`-`800DF20F` | `digipart_give` (`8004BE48`); `digipart_has` (`8004C010`) tests | H |
| `+0x50` / `+0x52` / `+0x54` | u16 ×3 | fusions done, cards used (+2 each), fusion mutations (all capped at 9999; the records screen's "Fusion Info") | `fusion_execute` (EVOSEG `801EE4B8`) | H |
| `+0x56` | u16 | the starter partner chosen at a new game (0-2); picks a screen resource (`800322AC`, also read by EVOSEG). The trainer leaves it alone | starter choice (OPENSEG `801EC450`) | M |
| `+0x80` | 0x288 × 3 | partner slots ([below](#partners-and-digimentals)); slot 0's card is `+0x2F8` | `partner_add` (`8004A0F8`) | H |
| `+0x2F9` | u8 | partner level (slot 0) | `partner_gain_exp` (KAWSEG `801F7600`) | M |
| `+0x2FA` | u16 | partner EXP (slot 0) | `partner_gain_exp` (KAWSEG `801F7600`) | M |
| `+0x818` | u16 × 32 | battle counters (capped at 999), role unknown (maybe wins per opponent), RAM `800DF9DC` | `battle_counters_add` (KAWSEG `801FCF78`) | M |
| `+0x1482` | u8 × 301 | card collection, one byte per card number, RAM `800E0646` | `collection_add_card` (`8004850C`) | H |
| `+0x15B0` | u16 × 6 per card | a random serial per copy (card × 12 + copy × 2) | `card_copy_serial` (`8004835C`) | M |
| `+0x2408` | 0x10C × 3 | saved decks ([below](#saved-decks-0x2408)) | `deck_store` (`8004979C`), `deck_fixup` (`8004945C`) | H |
| `+0x23CC` | 12 × u32 | city flags: bit n = city script register r(12+n), r12-r362 ([below](#city-flags-0x23cc)) | `city_flags_save` (SAISEG `801E36A8`) | H |
| `+0x23FC` | u8 × 9 | city script registers r363-r371 (small counters, e.g. r364) | `city_flags_save` | M |
| `+0x272C` | u16 × 3 | last battle's reward cards (`0xFFFF` = none) | `battle_rewards_pick` (`80048D68`) | H |

### Card collection byte

| Bits | Meaning |
|---|---|
| 0-2 | copies owned; `collection_add_card` stops at 6 |
| 0x10 | full (set with the count at 6) |
| 0x20 | "new": set on the first copy of a never-seen card, cleared by `collection_clear_new` (`800484AC`) |
| 0x40 | seen before (no "new" marker on the next first copy) |
| 0x80 | obtained |

`collection_add_card(player, card, count)` refuses cards 172-190 (returns -3). They are the armor
Digimon and the Digimon Adventure 02 partners (Flamedramon, Magnamon, Veemon ... Armadillomon),
which the game gives another way; the partner's own card is kept at one copy with the "full" flag
(`51`: seen, full, 1). The all-cards preset writes those 19 only while their byte is 0, so the
partner card and cards already owned are left alone.

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
`80036BEC` (small counters at `+0x36`/`+0x38` and `+0x276E`.. during battle), `80041650`
(`battle_result`: its record-update block writes the decks' `+0x106`/`+0x108`, see
[above](#deck-record-0x2408--deck--0x10c--0x1040x1060x108)), KAWSEG `801EF968` and `801ED064`.

## Progression flags

Story progress, event rewards, the partners and their Digimentals are not stored as items: they
are **MSD script registers** (the VM in [text-engine.md](text-engine.md) §5.1: `rN`, one `int`
each) that the city and Fusion Shop scripts test and set, saved as bits in `game_data`. The key
items (the Fusion Shop "data" rewards) are registers too. Disassembled with the walker of
`tools/text/msd.py`: `C:\EVENT\CITYnn.MSD` (city events, 12 files), `C:\EVENT\UNIT0n.MSD` (the
three Fusion Shop keepers, Andromon No.1-No.3). Op 9 is "skip the next record if the test holds",
so `IF rN != 1 / JUMP x` reads "if rN == 1 goto x".

### City flags (`+0x23CC`)

| Offset | What | Who | Conf. |
|---|---|---|---|
| `+0x23CC` 12 × u32 | bit n (word n/32, bit n%32; byte `+0x23CC + n/8`, bit n%8) = register **r(12+n)**, r12-r362 | `city_flags_load` (SAISEG `801E37A4`) copies bits → registers when a city script starts (write watch: 18 registers set on arrival in Flame City); `city_flags_save` (SAISEG `801E36A8`) copies them back (clears or sets each bit) before a host command leaves the script | H |
| `+0x23FC` u8 × 9 | registers r363-r371 as bytes (r364 = 6 in the recorded save) | same two functions | M |

`game_data_init` (`8002FD64`) clears both. The registers are the city script's own names; the
ones worked out:

| Register | Byte / bit | Meaning | Set by | Read by | Conf. |
|---|---|---|---|---|---|
| r156 | `+0x23DE` bit 0 | Izzy (光子郎, CITY05) beaten and his reward given | CITY05 script after the reward | CITY05 script | H |
| r266 | `+0x23EB` bit 6 | **Special Fusion data** (特別合成データ; Gatomon's reward, CITY08) | CITY08 script | every city script's start-up (+1 to `+0x14`); `city_fusion_flags` → `+0x2C` bit 0 | H |
| r267 | `+0x23EB` bit 7 (RAM `800E15AF` `0x80`) | **Mutation Detector** = "Fusion Mutation prediction data" (合成事故予知データ; Izzy's reward, CITY05) | CITY05 script (`08584 r267 = 1`) | start-up count; `city_fusion_flags` → `+0x2C` bit 1 | H |
| r294, r298, r301, r304, r307, r310 | `+0x23EF` bits 2, 6; `+0x23F0` bits 1, 4, 7; `+0x23F1` bit 2 | partner owned: Veemon, Hawkmon, Armadillomon, Patamon, Gatomon, Wormmon (`g_partner_flags`, SAISEG `801E0ED8`, by partner index 0-5: 294, 298, 301, 307, 304, 310) | `partner_flag_set` (SAISEG `801E35BC`) after the partner choice | city scripts (which partners to offer, which Digimental event to run) | H |
| r295-r297 | `+0x23EF` bits 3-5 | Veemon's Digimentals: Courage → Flamedramon (172), Friendship → Raidramon (185), Miracles → Magnamon (173) | city scripts (e.g. CITY10 `05FF8 r295 = 1`) | `digimental_sync` | H |
| r299/r300, r302/r303, r305/r306, r308/r309, r311/r312 | `+0x23EF` bit 7; `+0x23F0` bits 0, 2, 3, 5, 6; `+0x23F1` bits 0, 1, 3, 4 | the two Digimentals of Hawkmon (Halsemon 179 / Shurimon 188), Armadillomon (Digmon 189 / Submarimon 176), Patamon (Pegasusmon 180 / Baronmon 174), Gatomon (Nefertimon 181 / Tylomon 178), Wormmon (Shadramon 186 / Quetzalmon 177) | city scripts | `digimental_sync` | H |
| r317 | `+0x23F2` bit 1 | Ken's (賢, CITY09) partner gift given | CITY09 script | CITY09 script | M |

Unlocking one of these only needs its bit (they are read back when the next city script
starts), but a script may also use them for its own flow; see the Digimental caveat below.

### Partners and Digimentals

Partner slots: 3 records of `0x288` bytes at `game_data + 0x80 + slot*0x288` (slot 0 at `+0x80`),
per player (`+ player*0x2738`). Fields by the record's own offset (`game_data + 0x2F8` = slot 0's
card). What survives a save is the card, level, EXP, bonuses, parts and Digimentals: the working
copies and pointers are rebuilt from them when a save is loaded (`partner_refresh_all`,
`80049F10`, from OPENSEG `801F02D0`: level capped at 99, `+0x268` = the card's record, `+0x26C` =
the armed Digimental's record or the card's, then `partner_rebuild` per filled slot).

| Record offset | Size | What | Conf. |
|---|---|---|---|
| `+0x000` / `+0x134` | 0x134 × 2 | working copies of the card database records at `+0x268` / `+0x26C` (the Partner screen and battles read them), made by `partner_rebuild` (`8004AC98`): copied, `+0x58` set to 100 when 0, `+0x1E` += the HP bonus, the three attacks (`+0x20 + 28·i`) += their bonuses, the fitted Digi parts' effects (table `80071590`, 8 bytes per part), attacks clamped at 0, `+0x58` = 0 when the type byte `+0xE4` is 5-8 | H (code; the trainer's native rebuild of a fresh partner is byte-identical to the game's after a save/load) |
| `+0x268` / `+0x26C` | u32 × 2 | RAM pointers: card database record (`*801DB000 + card·0x134`) of the card / of the armed Digimental | H |
| `+0x270` | u16 | HP bonus from level-ups | M |
| `+0x272` | u16 × 3 | circle / triangle / cross attack bonuses from level-ups (each level-up adds 10 to HP or one attack, picked by `8004C260`, counted up by the battle-end EXP screen `partner_gain_exp`) | M |
| `+0x278` | u8 | partner card (0 = empty slot; an empty slot's other bytes are leftovers) | H |
| `+0x279` | u8 | level (1 on arrival, max 99) | H |
| `+0x27A` | u16 | EXP (`8004C248(level)`: EXP for the next level) | H (field), M (function) |
| `+0x27C` | u8 × 3 | Digi parts fitted (`0xFF` = none; `partner_fit_part`, `8004BD38`, needs the part owned) | H |
| `+0x27F` | u8 × 3 | the partner's Digimental cards received (0 = not yet), in `g_digimental_cards` order | H |
| `+0x282` | u8 | the armed Digimental's card (0 = none; `partner_armor_select`, `8004A8EC`) | H |
| `+0x283` / `+0x284` | u8 × 2 | bonuses from parts (effect kind 8), recomputed by `partner_rebuild` | M |

Other things that refer to a partner:

- the partner-owned city flag (r294 ... r310, [above](#city-flags-0x23cc)): what the city
  scripts test;
- the collection byte of its card: `partner_give` sets it to 1 copy, then `|= 0xF0` (obtained,
  seen, new, full);
- the saved decks: a Digimon entry whose card is a partner's points at that partner's slot
  record ([below](#saved-decks-0x2408)); battles find a partner by card
  (`partner_slot_of_card`, `8004A62C`, and `digimental_slot_of_card`, `8004ABC0`);
- nothing else was found: no "active partner" index (all three are shown and levelled; a deck
  takes the partner it holds).

- `g_partner_cards` (`800710F4`, u8[6]): Veemon 175, Hawkmon 182, Armadillomon 190, Gatomon 184,
  Patamon 183, Wormmon 187 (the "partner index" used below). **H**
- `g_digimental_cards` (`800710FC`, u8[6][3]): Veemon 172/185/173, Hawkmon 179/188,
  Armadillomon 189/176, Gatomon 181/178, Patamon 180/174, Wormmon 186/177. These 19 cards
  (172-190) are the ones `collection_add_card` refuses. **H**
- `partner_add(player, index, given)` (`8004A0F8`) fills the first empty slot (none when all 3
  are used or the partner is already there): pointers, card, level 1, EXP 0, parts `0xFF`,
  Digimentals and armed 0, bonuses 0, then `partner_rebuild`. Its only caller is `partner_give`
  (`8004A4F0`, `given = 1`): collection byte = 1 copy | `0xF0`, a copy serial, the rank update
  (`8002F484`) and the starter Digi part (`80071AD8`). Callers: the
  starter choice (OPENSEG `801EC450`) and the city partner choice (`partner_choose_task`,
  SAISEG `801F6058`), which offers what the script pushed with host `0x0B 10 n` (the list at
  SAISEG `801F7CE0`) and then sets the partner flag. **M** (code)
- So a player has **at most three partners** of the six: Ken's gift in CITY09 offers the
  partners not owned yet (Wormmon always). There is no "unlock all partners" flag: setting a
  partner's flag without a slot would only make the scripts believe it is owned.
- `digimental_sync` (SAISEG `801E39C0`), run by city host `0x0A 7` (opening the city **Menu**):
  for each owned partner, `digimental_give(0, index, n)` (`8004A6D8`) for each of its Digimental
  flags that is set (`g_digimental_flags`, SAISEG `801E0EE4`): record byte `+0x27F+n` = the card,
  collection byte `|= 0x50`. It also builds the 13-bit mask at `*80070C30 + 0xFB8` for the
  Partner screen. Seen at run time with the flags set: Veemon got 172/185/173, armed 172. **H**

### Saved decks (`+0x2408`)

Three records of `0x10C` bytes. **H** (code: `deck_fixup` `8004945C`, the starter deck
OPENSEG `801EC450`, `deck_entry_set` `800495D8`; seen in a save)

| Offset | Size | What |
|---|---|---|
| `+0x00` | u8 | 0 = unused, else in use |
| `+0x01` | 13 bytes | deck name ([name-entry.md](name-entry.md)) |
| `+0x10` | 8 × 30 | entries: u8 kind (0 Digimon, card < 191; 1 option, card − 191; 2 other, card − 293), u8 index in that kind, u16 card number, u32 RAM pointer to the card's data |
| `+0x100` | u16 × 6 | deck record counters: `+0x104` / `+0x106` / `+0x108` battles / wins / losses (capped at 999) |

### Deck record (`+0x2408` + deck × `0x10C` + `0x104`/`0x106`/`0x108`)

**H** (code, seen at run time). After a battle the game's record update (`battle_result`
`80041650`, block `800422B4`-`800422FC`) adds one to the winner's active deck `+0x106`
and the loser's `+0x108` (each capped: 999 kept), and KAWSEG `battle_counters_add`
(`801FCF78`) the per-opponent `+0x818` counters. `deck_store` (`8004979C`, RAM
`800498B8`-`800498DC`) keeps the same three capped. There is **no player-level total**:
what the VS and result screens show as "yours" is the active deck's record. The VS screen
(`vs_screen_draw`, KAWSEG `801F35D8`) copies deck `+0x106`/`+0x108` into its display struct
(KAWSEG `801FF1E4 + 0x760`-`+0x766`: P1 wins/losses, then P2's; `vs_record_draw`, KAWSEG
`801F03C8`) and draws them as `%4d` (wins + losses, the battle count), `%3d`, `%3d`.
`game_data_init` (`8002FD64`) zeroes the triple.

The pointers are rebuilt when a save is loaded (`deck_fixup` for each used deck, from
`decks_fixup_all` `800493C0`): Digimon → `*801DB000 + index·0x134`, or the partner's slot record
when a slot holds that card; option → `*801DAFF8 + index·0xDA`; other → `*801DAFFC + index·0x68`.

### Trainer: partner editor (General tab)

`src/platform/trainer_partners.cpp` (`set_partner`), written once when Enter or Space is pressed on a
partner row (the game is paused while the panel is open); saving in game keeps it. It relies on
the fields above:

- a new partner in a slot: what `partner_add` writes (level 1, EXP 0, no parts, no
  Digimentals, bonuses 0) and `partner_rebuild` with nothing fitted; its owned flag set and its
  collection byte `0xF1`. The next city Menu hands out its Digimentals whose flags are set.
- the partner it replaces: owned flag cleared, collection byte `0x40` (seen, none), and the deck
  entries that held it now hold the new partner (so a deck keeps a partner).
- a partner already in another slot: the two records are swapped (a partner is never in two
  slots: `partner_add` and the lookups by card assume one).
- refused: no save loaded (partner tables at `800710F4`/`800710FC` checked, `game_data` and
  `*801DB000` in RAM, a partner in slot 1); a gap before a filled slot or an empty partner 1;
  removing a partner that is still in a deck; a partner whose card sits in a deck as a plain
  card (possible only with the all-cards cheat).
- deck pointers are then redone as `deck_fixup` does.

Verified at run time (H): with the player's save in Flame City, Veemon → Gatomon in partner 1
and Patamon added as partner 2: the Edit Partner screen shows both at level 1, the flags
(r307, r304 set, r294 cleared) and deck 1's partner (184) held through opening the city Menu,
saving in game and loading the save again; the slot records the game rebuilt on load equal the
trainer's byte for byte. A swap (Patamon to partner 1) showed on the Partner screen too.
Not tried: a battle with a changed partner (the deck's entry points at the slot record, as
with a partner received in game).

### Trainer: deck record editor (General tab)

`src/platform/trainer_records.cpp` (`set_record`), written once when Enter is pressed on a
deck's wins or losses row (the game is paused while the panel is open); saving in game keeps
it. `Left`/`Right` step the choice by 1, `PgUp`/`PgDn` by 10 (clamped 0-999, the game's
cap); the row shows the choice and what the game has now (`W-L (battles)`). It writes
`+0x106`/`+0x108` of that deck only; the battle count (`+0x104`) is not stored separately
(the screens show wins + losses). Refused, with the reason on the status line: no save
loaded, or a deck that is not used yet.

### Fusion Shop flags (`+0x2C`)

The shop keeper's script (`C:\EVENT\UNIT0n.MSD`, run by `fusion_shop_host`, EVOSEG `801EACBC`)
has its own registers; r20-r29 are bits 0-9 of `game_data + 0x2C`: `fusion_flags_load` (EVOSEG
`801EB8B8`) sets the register when the bit is set, `fusion_flags_save` (EVOSEG `801EB914`) ORs set
registers back when leaving. `city_fusion_flags` (SAISEG `801E3628`) copies r266 → bit 0 and
r267 → bit 1 each time the player heads for the Fusion Shop (write watch: `+0x2C` 0 → 2 with r267
held by a cheat). **H**

| Bit | Register | Meaning | Conf. |
|---|---|---|---|
| 0 | r20 | has the Special Fusion data: the keeper announces special fusions (r14) | H |
| 1 | r21 | has the **Mutation Detector**: the keeper announces mutations (r13) | H |
| 2 / 3 | r22 / r23 | a keeper already commented on the special / mutation data | H |
| 4-9 | r24-r29 | keeper introductions done (r24-r26 No.1, r27/r28 No.2, r29 No.3); r27/r29 also pick the shop's level limit (script r12 = 0 / 2 / 3, host `0x0A 4`) | M |

### Fusion Shop: the roll, and what the Mutation Detector does

Card fusion (EVOSEG; `C:\EVO_PAK\n.PAK` holds its images): `fusion_card_select` (EVOSEG `801ED668`)
takes the first card, then on the second runs **`fusion_roll`** (EVOSEG `801EF738`) and
`fusion_check_full` (`801ED5A0`, r11 = -1 when the result is already owned 6 times). Partner cards
and levels above the shop's limit are refused there. `fusion_roll`: **H** (code; runs traced with
`DCB_TRACE_FUSION`)

1. The special-fusion table `g_special_fusions` (EVOSEG `801E1AFC`, 20 × {A, B, result, effect},
   either order): a match is a **special fusion** (kind 1, r14 = 1).
2. Else `r = rand() % 100`; a **mutation** when `r <= (A.value + B.value) / 10` (card byte
   `+0x18`, the "fusion value": 1-4 % for most pairs). Kind 2, r13 = 1, then `rand() % 100`:
   - `< 21`: a **Digi-Jewel**, card `273 + rand() % 12` (Digi-Garnet 273 … Digi-Turquoise 284);
     if six are already owned, Fake Sevens (card 200) instead;
   - `< 61`: **Fake Sevens** (にせセブンズ, card 200);
   - else a card whose fusion value is 1-3 above the pair's.
3. Else the normal result: a card of the pair's fusion value, its type from a 6×6 table at
   EVOSEG `801F2974` indexed by the two cards' types (M).

Results: card at `g_fusion_result` (EVOSEG `801F7FC0`), kind at `g_fusion_kind` (`801F7FD5`: 0
normal, 1 special, 2 mutation), script registers r11, r13, r14, r15/r16 (predicted type), r17
(kind). On "yes" (host `0x0A 19`), `fusion_execute` (EVOSEG `801EE4B8`) removes both cards
(`collection_remove_card`, `80048810`), adds the result and counts `+0x50`/`+0x52`/`+0x54`.

**The Mutation Detector does not prevent mutations.** Nothing in the fusion code reads it; only
the keeper's script does (UNIT00 `0219C`, UNIT01 `01E88`, UNIT02 `01AF8`): with r21 set and a
mutation rolled (r13), the keeper says "this fusion is going to mutate!" (合成事故が起きてしまい
そうだ) instead of predicting the result's type, then asks the same "fuse these two cards?"; "yes"
fuses and the mutation happens, "no" goes back to the second card, and picking it again rolls
again. So the item only lets the player *avoid* a mutation knowingly. The JP text of the reward
says the data lets you know about fusion accidents in advance (事故を未然に知ることができます); the
US line "It prevents mutations that might happen during Card Fusion" is a mistranslation.
Verified at run time (H): with r267 set by a cheat, the keeper greets with the "you got the
mutation prediction data" lines, warns on a mutation, and "yes" gave Digi-Sapphire (281) from
cards 300 + 299 (`+0x54` 0 → 1); the same fusion without the toggles gave a normal card (249).

What makes the Digi-Jewels hard is the odds: about 1-4 % per fusion for a mutation, then 21 % for
a jewel (1 in 12 for a given one). The trainer's fusion toggles below change that.

### Trainer toggles (General tab, "Outside battle")

`src/platform/trainer_toggles.hpp` (the list, cheat-file lines `!toggle <id> on|off`) and
`src/game/overrides/fusion.cpp` (the game side):

| Toggle | What it does | Conf. |
|---|---|---|
| `fusion_mutate` "Fusion: every fusion mutates (special kept)" | `dcb_fusion_roll` overrides `fusion_roll` (`config/SLPS-03101/overrides.json`, overlay EVOSEG) and re-runs the original until it rolls a mutation; a special fusion is kept. With the Mutation Detector the keeper warns every time; answer yes. | H (run: mutations after 52-647 rolls) |
| `fusion_jewel` "Fusion: mutations give a Digi-Jewel" | re-runs the roll until a mutation gives a Digi-Jewel the player does not have six of; alone it only changes fusions that mutated anyway, with `fusion_mutate` every fusion gives a jewel | H |
| `digimentals` "All Digimentals (given at the next city Menu)" | holds the 13 Digimental flag bits set (like a cheat, each frame, ORed into `+0x23EF`-`+0x23F1`); the next city **Menu** gives each owned partner its Digimentals (`digimental_sync`; nothing is given before that, so switching it on shows "open the city Menu to get them" on the status line) | H (run) |

Digimental caveat (M): scripts also read these flags for their own flow. CITY09's partner gift
shows "you got the X card" and sets r317 only when the new partner's first Digimental flag is
clear, so with the toggle on that message is skipped and the gift can be offered again (it still
adds the partner). The Digimental events themselves are skipped (their flags are already set).

## Trainer codes

`cheats/SLPS-03101.txt` (local): all cards ×4 (cards 172-190 only where not owned yet) and all 127 Digi parts,
as repeat codes over the tables above. See [../cheats.example.txt](../cheats.example.txt).
