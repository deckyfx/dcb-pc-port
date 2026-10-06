# Mods

[Home](Home.md)

Gameplay changes on top of the original game. They patch the game's own files as the game loads
them (`src/game/mods/`, `src/patch/mods.cpp`): nothing is added to the player's dumps or the
English data, and each mod can be turned off in `settings.ini`:

```ini
[mods]
boss_rematch = true
arena_save = true
player_rooms = true
```

## Boss rematch

The Battle Arena bosses can only be fought once: the usurpers who take over a city's arena, and
the story bosses. With this mod, a beaten boss waits in the Battle Cafe of the same city and can be
challenged again (Yes / No, then the battle), listed after the regular members:

| City | Boss | Deck | Appears after |
|---|---|---|---|
| Jungle City | Wormmon | Tryout Deck | beating him in the arena |
| Igloo City | Stingmon | Black Storm | beating him in the arena |
| Junk City | Shadramon | Evil Fire | beating him in the arena |
| Dark City | Digimon Emperor | Binding Chain | beating him in the arena |
| Infinity Tower | A | Darkness Wave | beating him (the last battle of the story) |

Their faces were already in the cafe's portrait sheet, in cells the cafe never lists (the
usurpers' cells belong to deck-info entries the game shows, or never shows, in the cafe). A cafe
gains regular members as the story goes on; a boss never takes a slot one of them uses (the mod
refuses such a slot), and Wormmon's own deck-info entry, shown until he is beaten, gives way to
his rematch entry. The cafe lines are the port's own.

Saves are not changed: the mod only appends to the city script and keeps every existing record
where it was, so a save made with it loads without it and the other way round.

## Arena saves

The Battle Arenas (Battle Arena, Extra Arena and the others) let you save only at a few battles
(the 4th and the 7th). With this mod every battle's menu has **Save** (Battle / Deck Data / Save),
in every city, story fights included (Sky City's Tailmon, the Digimon Kaiser). Saving there resumes
at that battle: its intro and menu again, the same opponent.
A's fight in Infinity Tower keeps the game's own flow (it recolours the whole screen).

A save made at one of these extra save points can only be loaded with the mod on (it resumes in
code the mod adds). Saves made at the game's own save points load either way.

## Player Rooms everywhere

Only Beginner City, Sky City and Wiseman Tower list **Player's Room** in their city menu ("Where
do you want to go?"). With this mod every city lists it, opened the way those three cities do,
and you are back in the city afterwards. The menu box has five rows: a menu that already lists
five places (Jungle City's once its Extra and Beet Arenas are open, and two other cities' at
similar points) stays as it is.

How it works (the cafe's script structure, the flags): [battle-cafe.md](../re/battle-cafe.md).
`dcb_patch --rematch <AREAnn.PAK> C/AREAnn.PAK <out>` applies it to one city file, for inspection.
