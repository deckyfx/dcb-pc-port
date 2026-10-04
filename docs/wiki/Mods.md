# Mods

[Home](Home.md)

Gameplay changes on top of the original game. They patch the game's own files as the game loads
them (`src/game/mods/`, `src/patch/mods.cpp`): nothing is added to the player's dumps or the
English data, and each mod can be turned off in `settings.ini`:

```ini
[mods]
boss_rematch = true
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

How it works (the cafe's script structure, the flags): [battle-cafe.md](../re/battle-cafe.md).
`dcb_patch --rematch <AREAnn.PAK> C/AREAnn.PAK <out>` applies it to one city file, for inspection.
