# Host-driven main loop (game on a fiber)

Status: **planned**. Nothing here is implemented yet except what "Today" describes.

Move the game's main code path onto a fiber so the PC side owns the frame loop. The game then
runs *inside* our program, one frame at a time, and stops at a clean point between frames.
This document covers why we want it, how it works, what could break, and what it enables
(pause, save states, trainers, custom menus such as a Custom Battle screen).

## Contents

1. [Today](#today)
2. [The change](#the-change)
3. [Risks and how they are handled](#risks-and-how-they-are-handled)
4. [Verification](#verification)
5. [What it enables (usage)](#what-it-enables-usage)
6. [Example: a Custom Battle menu](#example-a-custom-battle-menu)
7. [Rules going forward](#rules-going-forward)
8. [Roadmap](#roadmap)

## Today

| Piece | Where | How it works |
|---|---|---|
| Fibers | `src/runtime/src/fiber.cpp` | ucontext (Linux/macOS) or Win32 Fibers (Windows), mmap'd stacks with a guard page. |
| Guest tasks | `src/hle/system.cpp` | Each task the game's scheduler starts runs on its own fiber (`switch_context`, cookies `0xDCB00000|id`). |
| Main path | `src/game/main.cpp` | `psx_dispatch(entry)` runs directly on the OS thread. `System` adopts that thread as the first task. |
| Host work | `System::poll` → `on_vblank_` | At each VBLANK, whichever guest code is polling calls our host code: SDL events, input, present, audio, overlay, snapshots. Then `pace()` sleeps. |
| Interrupts | `System::deliver` | The game's handler runs nested on the current stack and leaves with a host `setjmp`/`longjmp` on that same stack; task switches happen after it returns. |

So the **game drives and the PC side rides along**: host code only runs when the game happens to
poll, on a game stack. Sometimes that's a task fiber, sometimes the OS stack.

## The change

The game gets a "main" fiber. The OS thread becomes a host-only loop:

```text
host (OS thread)                         game fibers
─────────────────                        ───────────
loop:
  system.resume_guest() ───────────────► run until the next VBLANK
                                          System::poll: vblank due → remember this fiber,
                        ◄───────────────  switch to the host fiber
  host_frame():
    events, input, present, audio,
    overlay, menus, pacing
```

Steps:

1. **Fiber stacks:** an optional large, lazily committed stack for the game's main fiber, 16 MB
   reserved (the OS main stack is 8 MB; task fibers use 1 MB). On Windows, `CreateFiberEx` with
   a large reserve and a small commit.
2. **System:**
   - At VBLANK, `poll()` raises the interrupt as today, then yields to the host instead of
     calling `on_vblank_`.
   - `resume_guest()` switches back to exactly the fiber that yielded. That can be the main
     path, any task, or even a stack that is inside an interrupt handler: the whole stack stays
     suspended as-is.
   - `pace()` moves to the host.
3. **main.cpp:**
   - A game fiber runs `psx_dispatch(entry)`.
   - Today's `on_vblank` lambda becomes `host_frame()` on the OS thread.
   - Closing the window leaves the loop instead of calling `std::exit` from inside game code.
4. **Errors:** C++ exceptions cannot cross fibers. The game fiber's entry catches them and hands
   the message to the host, which prints it and exits cleanly.
5. **Pause and frame-advance hotkeys:** a few lines once the loop exists (don't resume, or resume
   for one frame). This is the first user-visible proof.

Size: about 200–300 lines, in `fiber.cpp`, `system.cpp`/`.hpp` and `main.cpp`. The recompiler
and the generated C code are not touched.

## Risks and how they are handled

| Risk | Handling |
|---|---|
| Deep recursion overflows the fiber stack | 16 MB reserve (more than the OS stack today); the guard page faults loudly instead of corrupting memory. |
| Exceptions thrown inside game code | Caught at the fiber entry and forwarded to the host (step 4). |
| VBLANK arrives mid-interrupt or mid-task-switch | Safe by construction: the whole stack is suspended and the same fiber is resumed. |
| Interrupt `setjmp`/`longjmp` | Unchanged; it never crosses fibers. |
| Windows | Win32 Fibers already run the task fibers; smoke-test the cross build under Wine as before. |
| Sanitizers (ASan) with `swapcontext` | Not used today; would need fiber annotations if added. |
| Speed | Two context switches per frame: microseconds. |

## Verification

Yielding to the host does not change guest time (`ctx.cycles` advances exactly as before), so a
scripted run must produce **bit-identical frames** before and after the change.

1. On `main`, record a baseline: a headless run with `DCB_FAST=1 DCB_PAD_SCRIPT=...` through
   boot → FMV skip → title → menu → NEW game, and a hash of every `DCB_SNAPSHOT` frame.
2. After the change, repeat the run. **All hashes must match.**
3. `ctest --preset linux-debug`, then a hands-on pass: FMV, menu, memory-card save and load,
   Deck screen.
4. Windows cross build: run under Wine to the title screen.

## What it enables (usage)

Once the host owns the frame boundary, every feature below runs while the game is frozen at a
clean point. None of them has to cope with the game being halfway through a frame.

| Feature | How it works | Planned hotkey / switch |
|---|---|---|
| **Pause** | The host stops calling `resume_guest()`; the window keeps presenting and taking input. | `Pause` / `P` |
| **Frame advance** | While paused, resume for exactly one frame. | `F10` / `N` |
| **Fast-forward** | Skip pacing (today's `DCB_FAST=1`, but toggled live). | hold `Tab` |
| **Input record / replay** | Log pad state per frame; feed it back. Guest time is virtual, so a replay reproduces a run exactly, headless and across rebuilds. | `DCB_RECORD=run.inp`, `DCB_REPLAY=run.inp` |
| **Save states** (same run) | Copy out guest RAM, scratchpad, VRAM, SPU RAM, device and BIOS state, the task list, and the game fiber stacks; copy back to load. Valid within one process. | `F5` save, `F7` load, slots `1`–`4` |
| **Trainer / cheats** | Write guest RAM every frame (GameShark-style codes), or override recompiled functions in C for behaviour cheats. | `[cheats]` in `settings.ini` |
| **Memory search** | Pause, frame-advance and compare RAM between frames or save states to find the address of a value (HP, money, card counts). | debug overlay |
| **PC menus** | Settings, save states, trainer and custom modes drawn by the host on top of the frozen game. | `F1` |
| **Threaded rendering / audio** | Frame work can move off the game's critical path and overlap the next frame. This change enables it; it does not deliver the speedup by itself. | — |

Hotkeys are proposals. They will be configurable under `[hotkeys]` like `overlay = F3`.

### Running game code from the host

One capability matters for mods: the host can **request a guest call** ("run game function X
with these arguments") that the game fiber executes at the next frame boundary, on its own stack
and in a known state. Host code never calls recompiled functions directly: that would run game
code on the host stack, breaking the rule below.

## Example: a Custom Battle menu

Goal: a new mode where the player picks the opponent (and their deck) and the arena background,
then fights.

**Is it easier if we own the runtime? Yes, but the hard part is reverse engineering, not the
runtime.** The runtime work above makes the injection clean. The real effort is learning how the
game itself sets up and starts a battle. The approaches, from easiest to most authentic:

| Approach | What we build | What we must learn from the game |
|---|---|---|
| **A. Hook an existing battle** | A C override on the function that picks the opponent (and arena) for a normal fight: it returns the custom choice when the mode is active. The player enters any battle and gets the custom one. | The opponent- and arena-selection function(s) and their data. |
| **B. Host menu + direct start** | Our own PC menu (pause, pick opponent and arena from lists read from game data), then write the battle-setup variables into guest RAM and request a guest call to the game's "start battle" routine. | The battle-setup structure (opponent id, opponent deck, arena id, player deck) and a safe entry into the battle scene (KAWSEG overlay). |
| **C. Native in-game menu** | A new entry on the game's own menu, drawn with the game's text and sprite routines. It looks original. | The game's menu and UI system, scene state machine and font/sprite drawing calls. Most RE work. |

The suggested path is **A → B → C**:

- **A** proves we understand battle setup, with very little UI.
- **B** is the practical Custom Battle mode and needs the host loop (pause, clean state, guest
  calls at the frame boundary).
- **C** is polish, if we want it to feel native.

**Arena backgrounds.** The arena picture is game data loaded from disc (P.DRV / the battle
overlay's assets). Picking one of the game's own arenas is a matter of the arena id. Custom or
upscaled backgrounds would come from the asset pipeline (`docs/ASSETS_RIP_UPSCALE_REPACKAGE.md`).

**How the RE is done**, with the tools this plan gives us:

1. Record a replay that starts a normal battle.
2. Pause just before and just after the battle begins; diff RAM between the two save states to
   find the setup variables.
3. Confirm in Ghidra (who writes them, which function starts the KAWSEG battle scene).
4. Test by editing those values from the trainer before the battle starts.

## Rules going forward

- **Game code runs only on game fibers.** Host code never calls `psx_dispatch` or recompiled
  functions directly; it requests a guest call.
- **Host code touches game state only at the frame boundary**, while the game is suspended.
- **Nothing host-owned lives on game stacks.** Game stacks hold recompiled frames and pointers
  to long-lived runtime objects (context, `System`, `Bios`, `Mmio`) only. That is what makes
  in-process save states sound.
- **Debugging:** gdb and Windows debuggers handle fibers. A backtrace taken on the host side
  doesn't show game frames; stop inside the game fiber (or use `DCB_WATCHDOG`) to see them.

## Roadmap

1. Baseline frame-hash check on `main` (the verification run above).
2. Game on a fiber and the host loop; pause and frame-advance. The hashes must match.
3. Input record / replay.
4. Save states within a run (all device and HLE state serialisable).
5. Trainer: RAM codes from `settings.ini`, memory search in a debug overlay.
6. Custom Battle: approach A, then B.
7. Optional: save states that survive a restart (debug builds only: fixed stack addresses, build
   id check), rewind, in-game settings menu, threaded rendering.
