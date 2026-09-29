# Playing

[Home](Home.md)

## Settings

`settings.ini` (display, audio, key/gamepad bindings, hotkeys) is looked up in this order:
`DCB_SETTINGS`, the current directory, next to the executable, then the per-user file in home
(`~/.config/dcb-pc-port/` or `%APPDATA%\dcb-pc-port\`), where it is created on first run if none
exists. Keep one in the project root (gitignored) while developing.

It holds the initial window size, filtering, aspect, key/gamepad rebinding and volume. The window
is resizable and the picture fits it; `F8` switches between fit and integer scaling.
`DCB_FILTER=linear|nearest` and `DCB_SCALE=fit|integer` override the file's filter and scale mode
for one run.

Input: keyboard and gamepad map to the PS1 digital pad (timed SIO0 model); any key skips movies.

## Memory-card saves

Memory-card saves go to `saves/<serial>/card1.mcd` under the current directory: a raw 128 KB card
image that emulators and card managers also read. The game's memory-card file API (`bu00:`) is
implemented natively.

## Hotkeys

| Key (default) | `[hotkeys]` name | Action |
|---|---|---|
| `Esc` / `F1` (gamepad Start+Select) | `menu` | native pause menu |
| `F3` | `overlay` | performance overlay (FPS, game FPS, CPU/GPU load, audio queue) |
| `F4` | `trainer` | [Trainer](Trainer.md) |
| `F5` / `F7` / `F6` | `save_state` / `load_state` / `state_slot` | save state / load state / next slot |
| `F8` | `scale_mode` | switch fit / integer scaling |
| `P`, `Pause` | `pause` | freeze / resume the game |
| `N` | `frame_advance` | while paused, run one frame |
| hold `Tab` | `fast_forward` | run the game unthrottled |
| `F10` / `F11` / `F12` | `battle_p1` / `battle_p2` / `battle_reset` | Trainer battle actions |

The `F1` menu's Hotkeys page lists every hotkey and what it does. Pause, frame advance and
fast-forward come from the host-driven main loop: the game runs on fibers driven by the host
([design](../HOST_MAIN_LOOP.md)).

## Native pause menu

`Esc` or `F1` (`[hotkeys] menu`; gamepad Start+Select) freezes the game and opens the menu; Esc no
longer quits directly (Quit is a menu item with confirmation). Items:

- **Resume**
- **Save / Load state**: slots 1-4 with thumbnails and timestamps, same slots as the F5/F7 hotkeys.
- **Settings**: initial resolution 1x/2x/4x/8x with dimensions, scale mode, filter, aspect, volume,
  fullscreen — applied live and saved to `settings.ini`.
- **Controls**: keyboard + gamepad bindings and all hotkeys, always accurate.
- **Memory card**: back up `card1.mcd` to a timestamped copy, use any card file as the live card,
  switch between files.
- **About**: version, build, credits.
- **Quit**

Keyboard: arrows / Enter / Esc back. Gamepad: d-pad / south / east. The window is resizable and the
picture adapts (Resolution sets the initial size); fullscreen stays borderless desktop. Card
restores refuse while the game holds card files open; best done on the title screen.

## Save states

While playing, `F5` saves the game into the selected slot, `F7` loads it and `F6` selects the next
slot (1-4); a short notice confirms each ("State 2 saved", "Slot 3", "No state in slot 1"). They
work while paused too, and from the pause menu (which shows thumbnails). The keys are `save_state`,
`load_state` and `state_slot` under `[hotkeys]` in `settings.ini`. A state is bit-identical after a
load.

Limits:

- States live in memory for the current run only: they are gone when the game closes, and cannot be
  written to disk or moved to another machine (they contain host stack addresses; see
  [Persistent save states](#persistent-save-states) below).
- Memory cards are not part of a state: loading an older state does not undo a save written to
  `card1.mcd` since. Avoid loading a state taken in the middle of a memory-card save.
- Supported by the Linux build (glibc, x86-64 / ARM64) and the Windows build made with MinGW (the
  release `.exe`). An MSVC build or macOS shows "save states are not supported on this platform".
- Save states are refused while a [native movie](Movies.md) plays.

Scripted save-state checks (`DCB_STATE_SAVE_AT`, `DCB_STATE_STRESS`, ...) are described in
[Debugging and RE](Debugging-and-RE.md#scripted-save-state-checks).

## Persistent save states

Verdict: not reasonably feasible. A state saved to disk cannot be loaded after a restart, and
should not be attempted:

- The binary is PIE: code, statics, heap and stacks all land at different addresses every run
  (verified: three runs, three disjoint address sets).
- Game stacks at a frame boundary hold return addresses into our `.text`, pointers to long-lived
  heap objects (`Machine`, `Bios`, `Mmio`, `System`, the 2 MB guest RAM buffer), pointers to
  statics, and main-thread stack addresses (`tools/re/scan_stacks.py` on a real state: 61 code,
  45 heap, 42 binary-data, 33 main-stack values in 4 KB of stacks).
- Fixing the stacks (`MAP_FIXED_NOREPLACE`, verified working) is the easy 10%: the heap objects,
  statics and code addresses would all need fixing too (non-PIE build + fixed arenas), and then
  ASLR-disabled libc/SDL addresses inside `ucontext_t` and C++ exception state would still break.
- What works instead: memory-card backup/restore from the pause menu (robust cross-session save),
  plus a build-identity hash if states are ever written to disk (refuse foreign states clearly).

See [HOST_MAIN_LOOP.md](../HOST_MAIN_LOOP.md) for the full analysis.
