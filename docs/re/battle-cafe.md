# Battle Cafe and city battles

How a city's Battle Cafe lists its opponents and starts a battle, as the city script (MSD, see
[text-engine.md](text-engine.md) §5.1) and the city overlay SAISEG drive it. Found for the
boss-rematch mod ([Mods](../wiki/Mods.md), `src/patch/mods.cpp`); disassembled from the English
`C\AREA05.PAK` (Dark City) and `C\AREA11.PAK` (Infinity Tower) scripts.

## Script VM (`msd_vm_step`, `80021198`)

| Op | Size | Meaning |
|---|---|---|
| 5 | 8 | jump: `u16 5, u16 label (unused), i32 target`; the target is relative to the end of the 16-byte header |
| 7 | 12 | `u16 7, u16 reg, u16 kind, u16 is_reg, i32 value`: kind 0 `=`, 1 `+=`, 2 `-=`, 3 `*=`, 4 `/=`, 5 `%=`, 6 `= rand() % (value + 1)` |
| 9 | 12 | `u16 9, u16 reg, u16 cmp, u16 is_reg, i32 value`: **skip the next record** when the test holds: 0 `==`, 1 `<=`, 2 `<`, 3 `!=`, 4 `>`, 5 `>=` |
| 0x0A-0x0E | 4-20 | host command: `u16 op, u16 cmd`, then `op - 0x0A` arguments `{u16 is_reg, u16 value}` |

The scripts' branch idiom is `skip_if(r != v); jump L`: the jump is skipped unless r == v, so it
reads "if r == v, go to L". The header's `u32` at +8
is the script size (the VM stops there). Registers r12-r362 are saved as bits, r363-r371 as bytes
([save-data.md](save-data.md#city-flags-0x23cc)); a save made in a city also stores the script
position (host 0x0B cmd 6: `game_data + 0x30`), so a mod must not move existing records.

## City host commands (`city_event_host`, SAISEG `801E6618`)

The ones the cafe uses:

| Record | Effect |
|---|---|
| `0x0B cmd 2 (deck)` | battle: `g_city_battle_deck` (801F7CC0) = deck, the script yields; `city_battle_start` (SAISEG `801E8040`) spawns `city_battle_task` (`800440CC`), which copies DECK2.DEK record `deck` (0x68 bytes) and runs KAWSEG. On return r1 = the result: **0 = the player lost**, otherwise won |
| `0x0B cmd 3 (n)` | `cafe_list_add(n)` (SAISEG `801E3888`): appends cafe entry n to the list at 801F805D (count 801F8078); every 6 entries a page break (entries 0x10 / 0x11 are the page arrows) |
| `0x0A cmd 2` | shows the cafe list ("Who do you want to talk to?" follows) |
| `0x0A cmd 3` | the pick, into r2 (`-1` = cancelled) |
| `0x0B cmd 15 (n)` | music n (each cafe battle is followed by its city's cafe music: Dark City 131, Infinity Tower 128) |
| `0x0D cmd 0 (side, x, y)` | name box position for speaker `side` (0 / 1); every cafe line sets (0, 48, 10) and (1, 128, 10) |
| `0x0A cmd 6` / `cmd 4` / `cmd 5` | open a message box / show the text in r4 / close |
| `0x0A cmd 0 (97)`, `0x0B cmd 1 (n)`..., `0x0A cmd 1` | a choice list: items n (16 Yes, 17 No; 12-14 the opponent menu), the choice into r1 (1-based) |

## The cafe in a city script

1. **List.** A run of `cmd3(n)` records, each behind story-flag tests (e.g. an opponent appears
   once a chapter is reached; Nanimon behind a random roll), ends at the menu `cmd2()`. Several
   jumps lead to the menu as well as the record before it.
2. **Dispatch.** After the pick: `skip_if(r2 != k); jump section_k` for each listed entry, then
   `skip_if(r2 != -1); jump <leave>`.
3. **Sections.** Per opponent: the Talk / Battle / Deck info menu, lines, `cmd2(deck)`, the music,
   a line for a win or a loss (r1), and a jump back to the record after the menu (the list is not
   built again).

## Faces

`cafe_list_draw_faces` (SAISEG `801E8E50`) draws entry n from the city's face sheet: an 8-bit,
256x224 TIM uploaded to VRAM (640, 256) from the city PAK's image set (chunk kind 5, a `Tp` TIS),
cell (640 + (n % 4) * 32, 256 + (n / 4) * 56) halfwords, 63x56 pixels: 16 cells, n = 0-15.
Some cells hold faces the cafe never lists: Jungle City cell 15 is Wormmon (his deck-info entry,
listed while r56 != 1, i.e. until he is beaten), Igloo City cell 14 Stingmon and Junk City cell 6
Shadramon (deck-info sections the script has but never lists), Dark City cell 11 the Digimon
Emperor, Infinity Tower cell 14 A (and cell 15 Rosemon, listed in the final chapter).

## Bosses

| City | Boss | Deck | Arena battle | Beaten when |
|---|---|---|---|---|
| Jungle City (AREA02) | Wormmon | 10 | `cmd2(10)` | r56 = 1 (set on the win only; the arena usurper) |
| Igloo City (AREA03) | Stingmon | 14 | `cmd2(14)` | r87 = 1 (likewise) |
| Junk City (AREA04) | Shadramon | 18 | `cmd2(18)` | r119 = 1 (likewise) |
| Dark City (AREA05) | Digimon Emperor | 23 | `cmd2(23)` | r138 = 1 (set on the win; the city's dialogue tests it throughout) |
| Infinity Tower (AREA11) | A | 140 | `cmd2(140)` | r185 = 1 (met him) and r184 = 0 (r184 = 1 while the final event runs; cleared on the win) |
