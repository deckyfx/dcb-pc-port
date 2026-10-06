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

A script ends by running off its last record: leaving a city (Cross at "Where do you want to go?",
r1 = -1) jumps to the last few records (Jungle City: map music `cmd15(111)`, r0 = 0) and falls
off the end. Code appended to a script must therefore start with a jump to the new end.

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

## Arenas

An arena is a run of groups in the city script. Each group: per battle, a set-up (name boxes,
r10 = the opponent number, r9 = its name, r5-r8 / r11 its stats, host 0x0A cmd 13 for the
portrait), its lines, then the menu `cmd0(97); cmd1(13) Battle; cmd1(14) Deck info; cmd1()` with
tests for r1 = 1, 2, -1 (cancel: "leave the Arena?"); then the group's save block:
`cmd15(110)` (save music), `cmd6(code)` (save; the code is the location, 2 or 3 in arenas),
`cmd15(arena music)` and a jump back to the group's start, which re-dispatches on r268 / r269 /
r270 (set as battles are won). Only the battle that offers Save in its menu (item 15, the 4th;
also the usurper's) reaches its group's save block. Registers below r12 are not saved, so a save
must resume at a battle's set-up, not at its menu.

## Post-game

- **Apokarimon** (cafe slot 12 in Infinity Tower, AREA11) is listed while r226 (Wiseman Tower's
  arena course ending with Omnimon, deck 113, AREA10) and r248 == 0. Talking sets r264, which opens
  an Infinity Tower arena course ending with him (deck 114); the win sets r248 and r351. From then
  on he roams: r351-r358 pick the city (one each), and a cafe lists him (`cmd3(5)` elsewhere) when
  its register is set and r364 (+1 per battle) >= 10; a cafe win clears r351-r358 and r364 and
  picks the next city at random. The desert city (AREA06, Myotismon's cafe; the one that tests
  r355) lists him when `r364 >= 10` and `r355 != 0`. Infinity Tower's cafe tests
  `skip_if(r248 != 1); jump roaming; cmd3(12)`; the `postgame_visitors` mod makes that test
  `r248 == r248` (always skips the jump), so he stays listed there. A cafe win (AREA11 `0x9200`):
  r359 = 1, an S-Black Pack, his card the first time (r265) or on a 1-in-10 roll, then the next
  city.
- **Nanimon** is unlocked by r245, set when A first challenges the player in Infinity Tower (AREA11
  `0xe528`, the "?????????" scene before the last arena of the story; every city's Nanimon
  listing starts with `skip_if(r245 != 0)`). r363 counts his defeats and picks the city: Junk City (0, 7),
  AREA09 (1, 6), AREA06 (2, 5), Dark City (3, 8), Infinity Tower (4, 9), each on `rand(1)`; prizes
  at 5 and 10, the 10th sets r349 (Grand Sevens), after which only AREA06 lists him, on `rand(4)`.
  Every city's win handler does `r363 += 1`, r350 = 1 and an S-Option Pack (with r349: a 1-in-10
  S-Option Pack instead); only Infinity Tower's (where the rotation has him at 4 and 9) then
  gives the 5th win's Digi-Part 45 (`0x0B cmd16(45)`, r341) and the 10th's cards
  (`0x0D cmd1(288, 156, -1)`, r349, r336). The mod appends those two prizes to AREA06's handler
  and sends its jump back through them.
  The mod makes AREA06's r349 test never true (`r349 != r349`) and points its jump at the
  `cmd3(6)`: listed whenever r245.
- **After a win** a visitor's menu drops Battle until the city is re-entered: `skip_if(r359 != 1)`
  (Apokarimon) / `skip_if(r350 != 1)` (Nanimon) before a jump to the short menu; both registers are
  cleared at the script's start. The mod makes the test always true (`r == r`): r359 in AREA11,
  r350 in AREA06.
- **Diaboromon** (slot 11, AREA11) is listed when r225 (Wiseman Tower's course ending with
  WarGreymon, deck 137) and r189 (Sky City's ending with Magnadramon, deck 107, AREA08) and not
  r247. Talking sets r262 (an arena course ending with him, deck 109); the win sets r247.
- **The Black chain:** Igloo City (AREA03) reads the total wins at its start (`0x0A cmd16`, r1 =
  game_data+0x18) and sets r361 when r248 and `r1 >= 200`; its cafe lists BlackMetalGarurumon when
  r247 and r361. Beating him in Igloo City's arena (deck 116) sets r89; Beginner City (AREA00) sets
  r360 at its start when r89 and `r1 >= 300`, which lists BlackWarGreymon (deck 115, cafe slot 7).
  The `no_win_requirement` mod changes the `skip_if(r1 >= N)` before the r360 / r361 set to N = 0.
