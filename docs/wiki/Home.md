# dcb-static-recomp wiki

Static recompilation of **Digimon World: Digital Card Arena** (PS1, SLPS-03101) into a native PC
executable. The product is always the Japanese game, SLPS-03101; English text and art are grafted
onto it from the player's own US dump (SLUS-01328). No game data is ever part of this repository.

## Pages

| Page | What it covers |
|---|---|
| [Building](Building.md) | Repository layout, the disc → Ghidra → recompile → build workflow, CMake presets, `dcb.sh` |
| [Game Data](Game-Data.md) | Setting up from your two discs (`dcb --setup`, `dcb --import`), the first-run setup, where data is looked up, native file access, file overrides |
| [Playing](Playing.md) | `settings.ini`, memory-card saves, the native pause menu, save states, performance overlay, pause / frame advance / fast-forward |
| [Trainer](Trainer.md) | Built-in presets, battle actions (F10/F11/F12), GameShark-style cheat file, memory search |
| [Mods](Mods.md) | Gameplay mods: boss rematches in the Battle Cafe (`[mods]` in settings.ini) |
| [English Text](English-Text.md) | English font, card/deck text, city scripts and the text catalog built from the US dump |
| [Community Fixes](Community-Fixes.md) | romhacking.net `.xdelta` fixes applied to SLPS-03101 |
| [Textures](Textures.md) | Asset ripper, texture replacement packs, US images in the JP game, resizing sprites |
| [Movies](Movies.md) | Native MPEG-1 movie playback and how to rip / re-encode the movies |
| [Debugging and RE](Debugging-and-RE.md) | Input scripting, record / replay, scripted save-state checks, RAM write watch, coverage, environment variable reference |
| [Ghidra](Ghidra.md) | Ghidra project import, Ghidra MCP setup, mirroring recompiler discovery into Ghidra |

## Other documents in the repository

- [Hybrid English assets: research and plan](../HYBRID_EN_ASSETS.md)
- [Host-driven main loop (design, save-state analysis)](../HOST_MAIN_LOOP.md)
- [Reverse-engineering workflow](../RE_WORKFLOW.md)
- Subsystem notes: [docs/re/](../re/README.md) ([battle](../re/battle.md),
  [text engine](../re/text-engine.md), [save data](../re/save-data.md))
- [HLE layer](../../src/hle/README.md)
- [Cheat file template](../cheats.example.txt)
