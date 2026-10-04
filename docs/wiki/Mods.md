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

The Battle Arena bosses can only be fought once. With this mod, a beaten boss waits in the Battle
Cafe of the same city and can be challenged again (Yes / No, then the battle):

| City | Boss | Appears after |
|---|---|---|
| Dark City | Digimon Emperor | beating him in the arena |
| Infinity Tower | A | beating him (the last battle of the story) |

Their faces were already in the cafe's portrait sheet; the cafe lines are the port's own. Saves
are not changed: the mod only appends to the city script and keeps every existing record where it
was, so a save made with it loads without it and the other way round.

How it works (the cafe's script structure, the flags): [battle-cafe.md](../re/battle-cafe.md).
`dcb_patch --rematch <AREAnn.PAK> C/AREAnn.PAK <out>` applies it to one city file, for inspection.
