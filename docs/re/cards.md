# Cards: the database, the limits, the art

What the game assumes about its 301 cards, mapped to add new ones. **H** read in code or data,
**M** inferred. Saves and collection bytes: [save-data.md](save-data.md); card lists:
[card-lists.md](card-lists.md); battle structs: [battle.md](battle.md).

## Card numbers

The 301 cards are numbered by kind, and the kind of a number is decided by ranges all over the
code (constants 191 / 0xBF and 293 / 0x125, often 0x12D = 301):

| Cards | Kind (record `+2`) | Record size (JP / US) |
|---|---|---|
| 0-190 | 0 Digimon (172-190: the armor Digimon and the 02 partners) | 0x134 / 0x13C |
| 191-292 | 1 Option | 0xDA / 0xE2 |
| 293-300 | 2 Digivolve | 0x68 / 0x70 |

A number above 300 reads as kind 2 in `card_record_get` (`80048A8C`), `card_specialty_icon`
(`800489DC`) and `card_level_icon` (`80048A34`). Decks store a card as u8 kind + **u8 index**
(`deck_store` `8004979C`, `deck_entry_set` `800495D8`, `card_from_kind_index` `80048988`), so a
kind holds at most 256 cards; partner slots, Digimentals and the special fusions store card
numbers in one byte too. **H**

## The database: `B:\CARD2.CDD`

Header `"ADCD"` (US `"0ACD"`), u16 Digimon count 191, u8 Option count 102, u8 Digivolve count 8
(the older tools and [HYBRID_EN_ASSETS.md](../HYBRID_EN_ASSETS.md) call these arrays `digimon`,
`item` and `option`: `item` = kind 1 Option cards, `option` = kind 2 Digivolve cards),
then the three record arrays back to back, uncompressed (8 + 191·0x134 + 102·0xDA + 8·0x68 =
81904 bytes, the file). The US file differs only in the text block (4×21 bytes instead of 4×19)
and one condition byte of cards 5, 13, 128. **H**

`card_db_load` (`800481D8`) loads the file into a heap buffer sized by the file (`801DB004`) and
uses the records in place: Digimon at `801DB000`, Options at `801DAFF8`, Digivolve at
`801DAFFC` (bases computed from the header counts); it writes each record's card number into
`+0` (loops of 0xBF / 0x66 / 8). `card_record_get(n)` (`80048A8C`) picks the array by range. **H**

**Digimon record** (0x134):

| Offset | Field |
|---|---|
| `+0` u16 | card number (written at load) |
| `+2` | kind |
| `+3` | name (21) |
| `+0x18` | fusion value (0: cannot be fused) |
| `+0x19` | reward rarity (0: never dropped) |
| `+0x1A` | specialty (high nibble: Fire, Ice, Nature, Dark, Rare) / level (low nibble) |
| `+0x1B` / `+0x1C` | DP to digivolve / +P (DP given in the DP slot) |
| `+0x1E` s16 | HP |
| `+0x20 + 28·i` | attack i (circle / triangle / cross): s16 power, u16 skill A, u16 skill B (attack animation ids), name (22) at `+6` |
| `+0x74` | 2 conditions × 0x20 (evaluator KAWSEG `801EC68C`) |
| `+0xB4` | 3 effects × 0x10 (opcode at `+1`; executor KAWSEG `801EAC4C`) |
| `+0xE4` | cross-attack effect 0-15 (first attack, x to 0, counters, crash, eat-up HP, jamming, ×3 vs a specialty) |
| `+0xE5` | battle model id: `F:\%03d.PAK` (`battle_model_load` `8003082C`) |
| `+0xE6` | panel icon |
| `+0xE7` | effect text, 4 × 19 |

**Option record** (kind 1, raw `item`, 0xDA): `+0`-`+0x19` as above; conditions at `+0x1C`, effects at `+0x5C`, icon
`+0x8C`, text 4×19 at `+0x8D`. **Digivolve record** (kind 2, raw `option`, 0x68): `+0x18` / `+0x19` as above, `+0x1A`
a sub-id 0-7, text 4×19 at `+0x1B`. **H** (fields), M (some meanings).

## Where 301 cards are assumed

**Exe** (H): `card_db_load`; `collection_clear_new` (`800484AC`); `8004844C`;
`collection_seen_count` (`80049324`, into `game_data+0x12`); `deck_mastery_update` (`80048BCC`,
a 304-byte stack array); `battle_rewards_pick` (`80048D68`: allocations of 301×4 and 2×301×2);
`game_data_init` (`8002FD64`); `player_rank_update` (`8002F484`: ranges 0-171, 191-292,
293-300, per-specialty totals `80070C94` = {34, 35, 34, 36, 33, 110});
`digimon_record_by_model` (`80030578`, 0xBE).

**Overlays** (H): SUBSEG (Card Menu, Deck Edit): pointer tables `801F7178` / `801F7638` (u32 ×
301, static), sorts of 301 (`801EA5A4`, comparator `801EADF0`), per-deck counts `801EC9A0`,
totals `801ECC2C`, the 8 type pages with Option sub-ranges 191-227 / 228-264 / 265-292
(`801EC6E8`), "complete" marks (`801EDD04`: seen ≥ 301, copies ≥ 1608), auto deck (`801F4C10`,
a per-card table and option classes `801F5F6C`[card − 191]), the list icon (`801EC0B0`). EVOSEG
(Fusion Shop): `801F6F58` / `801F7418` (u32 × 301), allocations, sorts, jewel cards 273-284 and
card 200 hard-coded in `fusion_roll` (`801EF738`). OPENSEG (Battle with Friend, trade): 2×301 at
`801F65C0`, `801F59E0`, sorts. SAISEG records screen (`801E42E4`: seen × 1000 / 301). ENDSEG
(the ending's card roll, per-Digimon stats over 191).

**Collection and save** (H): `game_data` (0x2738 bytes per player; the heap block holds two, the
second for Battle with Friend) packs, back to back: `+0xD0C` u16×3 per Digimon (best damage),
`+0x1186` / `+0x1304` u16 per Digimon (KOs scored / taken), `+0x1482` u8×301 collection,
`+0x15B0` u16×6 per card (copy serials), ending exactly at `+0x23CC` (city flags). No room to
grow in place. `collection_add_card` (`8004850C`) has no upper bound: card 301 would write into
the pad byte `+0x15AF` and its serials into the city flags.

**Save file**: `BISLPS-03101_A/B/C`, 2 blocks. `save_copy_out` (OPENSEG `801F4180`) copies the
0x2738 bytes of player 0 into a 0x4000-byte buffer, `save_checksum_write` (`8002F444`) puts
XOR / 8-bit sum at `+0x2738`; the rest of the 0x3E00 data bytes (**0x16C6 bytes**) are written but
never read. Loading checks the checksum over `game_data+0x16` bytes (0x2738; a slot is listed only
with that value) and copies 0x2738 bytes back (`save_copy_in` `801F405C`). Checked on a real card.

## Fusion and rewards

**Fusion** (H): 20 special fusions `{u8 A, u8 B, u8 result, u8 effect}` (EVOSEG `801E1AFC`, either
order); otherwise the result kind comes from a 6×6 table (`801F2974`) of the two cards' kinds and
`fusion_find_by_level` (`801F008C`) takes the first card of that kind's range whose `+0x19` equals
`A+0x18 + B+0x18` (clamped 2..35). Ranges `801F2998`: {0,33}, {34,68}, {69,102}, {103,138},
{139,171}; Options {191,272} and {294,300} (`801F29AC`).

**Rewards** (H): no pack lists. `battle_rewards_pick(player, class)` sorts cards 0-300 into a
normal and a rare pool by `+0x19` within the class's bounds (`g_reward_classes` `80070F38`: 16
rows × 0x12 bytes); the rare chance is the partners' `+0x284` bytes summed. The class comes from
the city script (`801F7CED`).

## Art

| Asset | What | Indexed by |
|---|---|---|
| `B:\CARD\LCnnn.TIM` | the card picture, 64×64 8-bit, own palette (301 files) | card number (`"B:\CARD\LC%3.3d.TIM"`: EXE `800117D4`, EVOSEG `801E1DD0`, SUBSEG `801E2344`) |
| `B:\M_CARD.ARC` | mini cards 40×40 8-bit, level badge drawn in; u32 offsets[302] | card number; palettes shared per group (rows 249-255 at x 704: Digimon by specialty, Options, card 200 and 285-300), so new art must use its group's palette |
| `C:\OBJECT\DECK.TIS` | the 24×24 list icons, 6 sheets of 50 | computed from the number (`801EC0B0`: x = 576 + (n/50)·64 + (n%5)·12, y = 256 + ((n%50)/5)·24; card 300 apart at (560,480)); no slot above 300 |
| `F:\nnn.PAK` | battle model and animations | record `+0xE5` (190 ids). The disc has more PAKs than the records use (229-254 and 300: special fusion scenes and others); ids neither on the disc nor in a record: 1, 2, 15, 16, 29, 30, 43, 44, 62, 114, 116, 118-123, 125, 127, 130-135, 143, 144, 167, 175, 176, 203, 205, 210, 211, 216-219, 221, 224-228 |

The English port swaps US art by the content hash of the JP upload (`src/patch/art_swap.cpp`,
`hd_textures`); a new card has no JP original, so its art ships as it should look.

**Files the port serves**: `src/game/overrides/files.cpp` serves a name the disc does not have
from a loose file of that name, else as a copy of the file it stands for (`cards::alias`: a new
card's `F:\<model>.PAK`, and its `LCnnn.TIM` until it has art).

**Special fusion scenes** (H): the 20 specials' fourth byte (236-254, `0xEC`-`0xFE`) is the
`F:\nnn.PAK` played for the fusion (`FUN_8003063c(0, id, 0, 0)`, animation slot 7, from EVOSEG
`801E2A4C` through `801E2380`): the "unused" model ids. Each is its result's battle model with fewer
animations (236 = Imperialdramon's 181, 249 = MetalEtemon's 065; same header layout).

## What a new card needs

- **Data**: a record in `CARD2.CDD` and the lookups that pick a record by number (`card_record_get`
  and the inline copies in the overlays), the kind / number conversions, the deck kind+index.
- **Collection**: storage outside `game_data` (port-side, saved in the save file's unused tail),
  `collection_add_card` and every reader of `+0x1482` for numbers above 300.
- **Screens**: the Card Menu and Deck Edit tables and pages, the Fusion Shop and trade tables, the
  records' percentages.
- **Art**: `LCnnn.TIM`, an `M_CARD.ARC` entry, a list icon slot; a battle model id.
- **Getting it**: Wizardmon's codes (`CARDnnn`), rewards (`+0x19`), fusions (ranges and specials).

## The heap does not move (H)

`card_db_load` puts `CARD2.CDD` in a heap block the size of the file (`800CB1D4`, 0x13FF0 bytes),
and the next permanent block is `game_data` (`800DF1C4`; the collection at `800E0646`). A bigger file
moves game_data and everything after it (one extra record: to `800DF2F8`), which breaks every fixed
address: cheats, GameShark codes, the addresses in these notes. The allocator (`8001AE10`, block
table at `8008C428`: {address, size, owner} × 1024, first fit) has no slack to absorb it; the heap
ends at `801D7428`.

## What the port does for new cards

(`src/game/cards/`, `config/SLPS-03101/cards/*.ini`, [wiki](../wiki/Cards.md).)

- **Database**: `CARD2.CDD` is served unchanged; after `card_db_load` the database with the new
  records after the 191 Digimon (count raised) is rebuilt in **port RAM** (`0x80800000`, 1 MiB the
  console does not have, reached through the runtime's slow path) and `801DB000` / `801DAFF8` /
  `801DAFFC` point there. `801DB004` (the heap buffer) is only written by the loader.
- **By number**: `card_record_get`, `card_specialty_icon`, `card_level_icon`,
  `card_from_kind_index`, `deck_entry_set`, `digimon_record_by_model`: 301 + k ↔ Digimon index
  191 + k. `card_copy_serial` (`8004835C`) writes nothing for a new card (the serials of card 301
  would land in the city flags).
- **Collection**: card 301's byte is the pad byte `+0x15AF`; one new card until the collection
  moves to the save's spare tail.
- **Battle**: `best_damage_update` (KAWSEG `801ED064`) skipped and `battle_ko_check` (`801EF968`)
  wrapped for new cards (arrays of 191 by deck index: index 191 would land in the next array and
  the collection). Hand entries are {kind, deck index, u16 card, record*} at the battle player
  struct +0x10 + slot × 8; `battle_active_slot` (`80043168`) reads the active slot from +0x1C6.
- **Rewards**: `battle_rewards_pick` native, the same pools and rand() calls over every card
  (`DCB_VERIFY_REWARDS` compares it with the original: same cards, seed and collection).
- **Loops**: `config/SLPS-03101/immediates.json` makes the bound of loops that only touch each
  card's own byte (`8004844C`, `collection_clear_new`, `game_data_init`, the SUBSEG sort count, the
  Deck Edit available counts, the "complete" thresholds) a runtime variable.
  `collection_seen_count` and the records screen's percentage stay on 301.
- **Card Menu / Deck Edit** (SUBSEG, mapped in full; `card_screens.cpp`): the record tables
  `801F7178` / `801F7638` have 3 spare words each (cards 301-303); the list's row count is static
  data (`801F5CE4` = 301, list struct `801F5CC4` +0x20) set before the list is made; pages
  (`801EC6E8`, 8 × `malloc(164)`: 40 cards + terminator; room left: Fire 2, Ice 2, Nature 0, Dark 1,
  Rare 4); totals (`801ECC2C` would count a new card as Digivolve); the list icon (`801EC0B0`: no
  DECK.TIS slot above 300); the card pages (`801EE13C`, `801F191C`) read per-Digimon stats by card
  number. `801EC9A0` (per-card counts over 3 decks into `[801F7FC4]`) is dead code. The auto deck
  (`801F4C10`) finds records by number ranges inline and keeps to 0-300.
- **Art** (`src/patch/card_art.cpp`): the battle's setup (`80044B4C`) loads `B:\M_CARD.ARC` and
  uploads each deck card's mini card (its CLUT to its group's row, so a group shares one palette)
  to the hand slots; with the English art the texture replacer swaps those uploads (and the group
  palettes) for the US ones by hash, so a new mini card is quantized to the US palette and takes
  the US badge, keeping the JP palette bytes so that its palette upload is swapped too. The card
  picture (`LCnnn.TIM`) carries its own palette (uploaded to (704, 248)). The Deck Edit icon is put
  at (548, 480) in VRAM, drawn by `801EC250`.
- **Fusion Shop** (EVOSEG, not done): its tables `801F6F58` / `801F7418` (built by
  `fusion_card_table_init` `801E9024`); the first has 3 spare words, but entry 301 of the second is
  `801F78CC`, the pointer to a per-card count buffer (read by `fusion_execute` and 9 others), so a
  new card there needs the tables moved. `fusion_roll` and the result screens read
  `801F7418[result]`.
