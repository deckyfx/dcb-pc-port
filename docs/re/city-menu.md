# City menu, host screens and battles from a city

How a city (SAISEG, overlay base `801E0B30`) runs its menu, opens its own screens and starts a
battle; mapped for the port's Custom Battle ([below](#custom-battle)). The script VM and the
Battle Cafe are in [battle-cafe.md](battle-cafe.md); the records a battle writes in
[save-data.md](save-data.md#opponent-records-0xa90). Confidence: **H** read in code (and seen
at run time where noted), **M** inferred.

## The script VM and the host

`msd_vm_step(vm, regs)` (`80021198`) runs records until a host command (ops `0x0A`-`0x0E`), whose
op, command and decoded arguments it leaves at `vm+0x18`, `+0x1A`, `+0x1C + 2i` (a register
argument, `{1, n}`, is read here: every host command takes them). It returns 1 at a host command,
0 at the end of the script, -1 while `vm+0x24` (a pause) is set. **H**

`city_event_host` (`801E6618`) loops on it and switches on the command (op `0x0A`: jump table
`801E106C`, 23 commands; op `0x0B`: `801E10CC`, 22). Most commands continue the loop; the ones
that hand over to something else return. Register **r0 is "the script runs"**: the battle,
save and map commands set it to 0 and an exit mode (`801F7CDD`), and so does the default case,
so a command the host does not know ends the script (the city then leaves for the map). **H**,
seen at run time.

| Address | Meaning |
|---|---|
| `801F7E80` | -> the host state: `+4` the VM state, `+8` the registers (r1 at `+4`) |
| `801F7CDE` | the city mode (`city_frame`, `801E7594`): 1 the city menu, 10 a task runs (the script waits until it is 0 again) |
| `801F7CDD` | the exit mode, read by the city's main task (`801E7A9C`, table `801E11AC`): 1 battle (`800440CC`), 2 map, 3 save, 4 / 6 others |
| `801F7CC0` | the exit argument (the deck for a battle) |

## The city menu

"Where do you want to go?" is `0x0B cmd0(120)` (`city_menu_open`, `801E2E08`; 97 = a 3-row choice
list instead), one `0x0B cmd1(n)` per place (`city_menu_add_item`, `801E2FC4`: no bounds check;
the box has 5 rows), then `0x0A cmd1()`, which sets the mode to 1. **H**

- **Probe.** Before showing it, the host runs `city_menu_probe_item` (`801E3DC4`) for each place:
  r1 = the place's position, the script is run ahead to its next host command, and a place whose
  command is `0x0A cmd 14` (a no-op marker) is greyed out (unselectable). So a place's command is
  *looked at* when the menu opens. **H**, seen at run time.
- **Input** (`city_menu_step`, `801E3358`, states slide in / input / slide out / free): Up/Down,
  Circle puts the 1-based position in r1, Cross -1. **H**
- **Labels** are sprites from a label sheet in every city PAK: 88×176 at VRAM (768,0), 16 px a
  row, 4-bpp, palettes (512,239)-(512,242) for normal, highlighted and greyed. Row n = place n:
  0 Player's Room, 1 Menu, 2 Battle Cafe, 3 Battle Arena, 4 Extra Arena, 5 Beet Arena, 6 Haunted
  Arena, 7 Fusion Shop, 8 / 9 Yes / No, **10 a blank button** (JP and US art). The choice list's
  sheet is 64×144 at (792,0), places 12-20 (Battle, Talk, Deck info, Save, Yes, No, ...). **H**
- **Drawing:** `city_menu_draw` (`801E33A4`, from the city's draw callback `801E81E8`): each item
  is a sprite object (`sprite_new`, `801EEC70`; 0x90 bytes, two POLY_FT4 at `+0` / `+0x28`, the
  drawn one picked by `8007C0D4`), drawn through the GTE at OT layer 0x21 (the box 0x20). Item
  sprites at `801F8910 + 4i`, count `801F7C54`, type `801F7C52` (1 city). **H**

## Screens a host command opens

A host command that shows a screen (Wizardmon's spell, `0x0A cmd 15`: close the message window
`801F818C` with `window_resize(win, -1)`, sound 4, mode 10, spawn the keyword task `801EE704`)
returns with r0 untouched; the task runs its own loop and, when done, puts its answer in r1,
sets the mode to 0 and reopens the message window (`801E3CC0(1)`). **H**

- **Tasks:** `task_spawn(0, -1, 0, stack, entry, arg, parent, 0, 0)` (`80014AEC`), `task_id()`
  (`800149E4`), `task_sleep(frames)` (`80014D3C`).
- **Per-frame draw hooks:** a null-terminated list at `8007C0E0`; `draw_hook_add(fn)` (`80016988`),
  `draw_hook_remove(fn)` (`800169C4`). The keyword screen's hook (`801EE6A4`) draws its three
  windows.
- **Windows** (0xA0 bytes): `window_init(win, rect, from, content, style, frame, alpha, frames)`
  (`80016D54`; `from` -1: it opens from its centre over `frames` frames), `window_resize(win,
  rect | -1)` (`80017084`; -1 closes), `window_draw(win, callback, ot)` (`8001723C`; clips to the
  window and calls `callback(win, ot_entry)`: content x / y at `win+0` / `+2`, OT index at
  `+58`). The keyword screen's windows: `801F7A20`, `801F7AC0`, `801F7B60`.
- **Cursors:** `cursor_init(c, rect, -1)` (`80019240`), `cursor_set_target(c, rect)` (`800193CC`),
  `cursor_draw(c, ot)` (`80019448`); the keyword grid's at `801F7A70`.
- **Text:** `text_draw_grey(x, y, clut, prop, ot, str)` (`8002ADC8`; codes and button icons:
  `*b0` circle, `*b1` triangle, `*b2` cross, `*b3` square).
- **Sound:** `8002DCD8(n)` (0 confirm, 1 cancel, 2 move, 4 open).

## A battle from a city

`0x0B cmd2(deck)` (`801E6CE4`): sets the resume flag (`*(80070C30)+0xFA4` `+0x1A9` = 1), the
deck, the script position after the record (`FA4+0x198`), r0 = 0 and exit mode 1. The city's
main task then saves the flags (`801E36A8`) and spawns `city_battle_task` (`800440CC`): it loads
KAWSEG and `DECK2.DEK`, sets `*(80070C30)+4` = the deck and `+8` = its record, runs the battle
(`80041650`: the Deck Select, the records, the rewards), adds to the totals (`game_data+0x18` /
`+0x1A`) and the rank, and reloads the city. Back in it (`801E8B8C`), the script resumes after
`cmd2` with **r1 = 1 won, 0 lost**; the music is the script's (`cmd15` after `cmd2`). **H**, seen
at run time.

Back in the city, `city_scene_init` (`801E88C4`) restores the scene the battle was started in
(`801F7CEA` = `FA4+0x1A8`, set by `0x0B cmd8(n)`: 1 the cafe, 0 Player Rooms, 0xFF the city) and
the "back from a battle" state: `801F7CDC` = 1 and `801F7CE4` = 9 (the cafe scene task `801EA518`
shows the player's and the opponent's portraits); the cafe's `0x0A cmd3()` (its list pick) sees
`7CDC` and closes it (`7CE4` = 7). A plain entry sets `7CB4`-`7CB6`, `7C88`, `7C8C`, `7CDC` to 0
and `7CE4` = 4. The game only battles from a talk scene. **H**

Deck 0 runs the tutorial; decks 142-158 are the player's preset decks. The VS screen loads
`B:\MATCH\NNN.ARC` (NNN = the deck): its TIM 12 is the opponent's 128×112 portrait.

## Custom Battle

The port's mod (`[mods] custom_battle`): Beginner City's menu gets a 5th place.

- **Script** (`patch::mods::add_custom_battle`): item 10 (the blank button) after the places; its
  position runs `0x0A cmd 0x40`, then `skip_if(r1 != -1)` back to the menu's start, else
  `cmd2(r1)` and the script's start (the city, its music). Appended only.
- **Command** (`src/game/overrides/custom_battle.cpp`): the override of `msd_vm_step` turns
  `0x0A cmd 0x40` into `0x0A cmd 15`, the host's own screen command, so the host opens a screen
  and returns with the script still running; the keyword task's override (`801EE704`) runs the
  Custom Battle screen instead of the keyword entry. Not while the menu probes (the override of
  `city_menu_probe_item`).
- **Screen:** the decks 1-141 whose record (`deck_record_index`) has `game_data+0xA90` bit 15, one
  per opponent and deck name (Betamon's cafe decks share the tutorial's record 0), from the English `DECK2.DEK`
  (owner, deck name, prize pack, EXP) and the player's record against each; the keyword screen's
  windows and cursor; a draw hook and window callbacks that are native code at reserved guest
  addresses (`psx::register_native`, `kNativeBase` `9FF00000`: `psx_dispatch` runs them).
- **Back from the battle:** it was started in the city scene (0xFF), where nothing closes the
  "back from a battle" state; the override of `city_scene_init` sets a plain entry's state then.
- **Portrait:** entry 12 of the deck's `B:\MATCH\NNN.ARC` (the VS screen's 128×112 8-bit portrait,
  normally at VRAM (320,112), palette (320,255)), uploaded through the GPU's transfer command
  (GP0 A0h, so the texture replacer gives it the US art: it matches uploads by content) to VRAM
  the city leaves free (picture (512,384), palette (0,248), found with `DCB_SNAPSHOT_VRAM`), and
  drawn as one sprite from the text primitive pool (`dcb::text_draw_image`).
- **Label:** "Custom Battle" is composed into the label sheet's blank row 10 in VRAM from the
  English sheet's own letters (C of Cafe, u of Menu, s of Player's, t of Battle, o and m of Room,
  then Battle; a letter = the texels that differ from row 10's background, which every row
  shares), so the game draws it in its palettes (normal / highlighted / greyed) like the other
  places; once per sheet load, from the `city_menu_draw` override (`hle::Gpu::poke_vram`). With
  the JP sheet (no English "Battle Cafe" in row 2) the words are written over the button instead.
