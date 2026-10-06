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
desert_visitors = true
no_win_requirement = true
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

## Post-game visitors in the desert city

**Apokarimon** first waits in Infinity Tower's Battle Cafe (after Wiseman Tower's arena course
that ends with Omnimon); talking to him opens Infinity Tower's Battle Arena, which ends with him.
Once he is beaten there he wanders: he sits in one of seven cities' cafes, picked at random, and
only after ten more battles; beat him and he moves on. **Nanimon** goes round five cities' cafes in
turn (one more city per win, each visit on a coin flip) from the point A challenges you in
Infinity Tower, and after his tenth defeat he stays in the desert city on a dice roll. After a win,
either one refuses another battle until you leave the city.

With this mod both are always in the desert city's Battle Cafe (the city with Myotismon):
Apokarimon from the moment he is beaten in Infinity Tower's arena, Nanimon from the point the game
unlocks him, and both can be fought again right away. The rest of the game is unchanged (they also
still show up where the game sends them; Nanimon's prizes at 5 and 10 wins still come).

## No win grinding for the last opponents

The post-game chain, in Infinity Tower and then two cities' Battle Cafes:

1. Wiseman Tower's arena courses that end with WarGreymon and with Omnimon, and Sky City's that
   ends with Magnadramon, bring **Diaboromon** and **Apokarimon** to Infinity Tower's Battle Cafe.
   Talking to each opens an Infinity Tower Battle Arena course that ends with him.
2. Beat both courses: **BlackMetalGarurumon** joins Igloo City's Battle Cafe (the game also waits
   for 200 total wins).
3. Beat him in Igloo City's arena: **BlackWarGreymon** joins Beginner City's Battle Cafe (the game
   also waits for 300 total wins).

This mod drops the win counts only; every story step above is still needed, so nothing can show up
early. The city checks when you enter it: walk in (or out and back in) after the step.

How it works (the cafe's script structure, the flags): [battle-cafe.md](../re/battle-cafe.md).
`dcb_patch --rematch <AREAnn.PAK> C/AREAnn.PAK <out>` applies it to one city file, for inspection.
