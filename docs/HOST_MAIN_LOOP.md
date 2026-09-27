# Host-driven main loop (game on a fiber)

Status: **implemented**: the host loop, pause, frame advance, fast-forward, input record/replay,
save states within a run ([Save states](#save-states)) and the trainer (cheats + memory search).
Custom menus are still planned. "Today" below describes
the design *before* this change; "The change" is what the code does now.

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
6. [Save states](#save-states)
7. [Example: a Custom Battle menu](#example-a-custom-battle-menu)
8. [Rules going forward](#rules-going-forward)
9. [Roadmap](#roadmap)

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
| Windows | Smoke-test the cross build under Wine as before. Since save states, the MinGW build switches fibers with its own assembly routine (see [Save states](#save-states)); MSVC builds still use Win32 Fibers. |
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

| Feature | How it works | Hotkey / switch |
|---|---|---|
| **Pause** | The host stops calling `resume_guest()`; the window keeps presenting and taking input. | `P`, `Pause` (`[hotkeys] pause`) |
| **Frame advance** | While paused, resume for exactly one frame (hold to step). | `N` (`[hotkeys] frame_advance`) |
| **Fast-forward** | Skip pacing while held (like `DCB_FAST=1`); the pacing clock is resynced afterwards so nothing is slept off or rushed. | hold `Tab` (`[hotkeys] fast_forward`) |
| **Input record / replay** | Log pad state per frame; feed it back. Guest time is virtual, so a replay reproduces a run exactly, headless and across rebuilds. | `DCB_RECORD=run.inp`, `DCB_REPLAY=run.inp` |
| **Save states** (same run) | Copy out guest RAM, scratchpad, VRAM, SPU RAM, device and BIOS state, the task list, and the game fiber stacks; copy back to load. Valid within one process. See [Save states](#save-states). | `F5` save, `F7` load, `F6` slot `1`–`4` |
| **Trainer / cheats** | Write guest RAM every frame (GameShark-style codes), or override recompiled functions in C for behaviour cheats. | `F4` panel, `cheats/<serial>.txt` |
| **Memory search** | Pause, frame-advance and compare RAM between frames (or around a save state) to find the address of a value (HP, money, card counts). | `F4` panel, Search page |
| **PC menus** | Settings, save states, trainer and custom modes drawn by the host on top of the frozen game. | `F1` |
| **Threaded rendering / audio** | Frame work can move off the game's critical path and overlap the next frame. This change enables it; it does not deliver the speedup by itself. | — |

Pause, frame advance and save states exist and are configurable under `[hotkeys]`, like
`overlay = F3`; the other hotkeys are proposals.

### Running game code from the host

One capability matters for mods: the host can **request a guest call** ("run game function X
with these arguments") that the game fiber executes at the next frame boundary, on its own stack
and in a known state. Host code never calls recompiled functions directly: that would run game
code on the host stack, breaking the rule below.

## Save states

Status: **implemented** for states within one run (`F5` save, `F7` load, `F6` next slot, 4 slots
kept in memory). Code: `src/runtime/include/psx/state.hpp` (stream), `src/hle/save_state.*`
(whole machine), `src/game/save_states.*` (slots, hotkeys, debug triggers), a
`save_state`/`load_state` pair in every class listed below.

### Format

A state is a byte stream of chunks, each `tag | version | size | payload`. Every class writes its
own chunk with explicit fields (no raw copy of an object that holds pointers; plain structs of
scalars such as SPU voices are copied as values). The reader checks tag and version, never reads
past a chunk, and requires each chunk to be consumed exactly, so a layout change without a version
bump fails loudly instead of misreading the rest (`psx::StateError`). A header chunk carries a
random per-process session id: a state from another run is rejected. Loading is all-or-nothing:
the current machine is saved first and put back if the state turns out to be bad halfway.

| Chunk | Class | Contents |
|---|---|---|
| `DCBS` | `hle::save_guest` | format version, session id |
| `CPU ` | `psx::Machine` | GPRs, hi/lo, pc, COP0, GTE data/control, poll budget, guest cycles, 2 MB RAM, 1 KB scratchpad |
| `MMIO` | `hle::Mmio` | I_STAT/I_MASK, 7 DMA channels, DPCR/DICR, 3 timers, plain registers, SPU sample clock, audio not yet taken, display-flip and MDEC-transfer counters (the FMV skip reads it), then: |
| `GPU ` | `hle::Gpu` | 1 MB VRAM, command FIFO and transfer progress, polyline state, drawing environment, display control, GPUREAD latch, field |
| `SPU ` | `hle::Spu` | registers, 512 KB sound RAM, 24 voices (ADPCM position and history, ADSR, sweeps), key on/off, ENDX, IRQ, noise, capture position, CD audio queue, reverb buffers and phase |
| `CDRM` | `hle::CdRom` | registers and FIFOs, data FIFO and position, drive mode/status, seek and read position, sector timing, queued responses (with due times), both sector buffers, ack hold-off, XA filter, volume matrix, mute, and the XA decoder (`XADC`: ADPCM history, resampler ring and phase) |
| `SIO0` | `hle::Sio0` | port registers, byte in flight and its ACK timing, pad protocol position, buttons |
| `MDEC` | `hle::Mdec` | command state, quant/scale tables, block being decoded, macroblock, output FIFO |
| `BIOS` | `hle::Bios` | heap bookkeeping, 16 event blocks, interrupt chains, HookEntryInt, ChangeClearPad/RCnt, and the card file system (`CDFS`: open files, positions, block chains, errors, firstfile cursor) |
| `SYS ` | `hle::System` | VBLANK counters, `in_irq_` and the interrupt `jmp_buf` address, dispatcher cache, task list (cookie, registers, resume request, dead flag) with each task's fiber image |
| `HOST` | `dcb::HostFrameState` | pad frame counter (indexes `DCB_PAD_SCRIPT` and record/replay), FMV-skip state |

Not saved, on purpose: memory-card images (files on disk, like other emulators), the disc (read
on demand, no cache), host statistics and log throttles, pacing (restarted from the loaded guest
time), the snapshot frame counter (monotonic).

### Fibers

At a frame boundary every game fiber is suspended: the one that reached VBLANK inside
`System::yield_to_host()`, every other task inside `System::switch_context()`. A fiber's native
state is its stack bytes (from the saved stack pointer, minus the red zone, to the top) plus its
saved registers. Restoring must put them back **at the same addresses**, because the stack holds
pointers into itself (frame links, the interrupt `jmp_buf`, locals' addresses).

- **Stacks are never unmapped.** A destroyed fiber's stack goes to a free list and is reused by the
  next fiber of the same size (`src/runtime/src/fiber.cpp`). So every stack a state refers to still
  exists: free, or owned by a live fiber.
- **Load** destroys all current task fibers (their stacks return to the free list), then
  `Fiber::restore()` claims each saved stack back by address, copies the bytes in and installs the
  register context. Tasks created or destroyed since the save are handled by this: the task list,
  `Task` objects and cookies are rebuilt from the state.
- **`Task` objects are recreated**, so no frame may keep a `Task&` across a switch:
  `switch_context()` keeps only its cookie and looks itself up again when resumed.
- **ucontext (Linux):** the context is the `ucontext_t`; glibc/x86-64 keeps a pointer to the FPU
  save area inside it, which is relocated after the copy. A fiber that never ran (a state taken
  before the first frame) restarts from its entry, which the image records.
- **Memory:** stacks are reserved address space committed as used; the free list only grows to the
  largest number of fibers alive at once.

**Windows decision: supported, with our own context switch.** Win32 Fibers keep the context in
OS-owned memory at an undocumented layout, so they cannot be captured. The MinGW build (the
release `.exe`) now uses a short assembly switch of its own instead (`dcb_fiber_switch`): it saves the
Win64 callee-saved state (rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15, MXCSR, x87 control word) **and
the thread's stack bounds in the TIB** (StackBase, StackLimit, DeallocationStack), all on the
fiber's own stack, so a state needs only the stack bytes and the stack pointer. Swapping the TIB
bounds is what keeps SEH unwinding (C++ exceptions) and stack checks working on a fiber stack;
stacks are committed up front with a no-access guard page. Checked under Wine: the fiber unit
test (including an exception thrown and caught on a fiber) and every scenario below, bit-identical
to Linux. An MSVC build keeps Win32 Fibers and reports that save states are unsupported (it would
need the same switch as a MASM file). To do: a pass on real Windows.

### Yield hazard audit

A frame boundary can be reached from any guest code (`psx_poll` at loop back-edges) and from these
HLE paths: `Mmio::read` → `System::io_poll`; `Bios::b0_wait_event` → `System::idle`; guest code
called from the host: `System::deliver` (VBLANK event and the libetc handler, with a `setjmp`
buffer on the stack), `Bios::run_interrupt_chains`, `Bios::deliver_event` (event callbacks, also
from card operations), `psx::call_guest`, the task overrides (`src/game/overrides/tasks.cpp`), and
the task entry points. Everything those frames hold at that moment is restored from the state as
stack bytes. Heap memory they own is not, so after a load the frame would own freed or reused
memory. Found and fixed:

| Frame | Problem | Fix |
|---|---|---|
| `Bios::run_interrupt_chains` | iterated `std::vector int_handlers_` while calling guest code | fixed-capacity array (32); each priority pass walks a copy on the stack |
| `Bios::b0_write` / `b0_read` (card files) | a `std::vector` buffer alive while `file_async_event` ran guest callbacks | buffer scoped to end before the event |
| `System::switch_context` | `Task& me` used after the fiber is resumed | keeps the cookie, looks the task up again |
| `System::task_main` | `Task& me` in scope for the task's life | scoped to the start of the task |

Checked and safe: `System::deliver` (register arrays and the `jmp_buf` are stack data; `irq_env_`
is saved), `call_guest` (arrays), `b0_wait_event` (pointer into the `Bios` object's event array,
which is loaded in place), `deliver_event` (range over that same member array), `Mmio::read`, DMA
(its vectors are freed before any poll), `Bios::call` (map iterator into the fixed handler table),
the task overrides (scalars), generated code (scalars only; no C globals). Rule, in
[Rules going forward](#rules-going-forward): no heap-owning locals in a frame that can reach a
frame boundary.

### Verification

`DCB_STATE_SAVE_AT=N DCB_STATE_LOAD_AT=M` (headless, `DCB_FAST=1`, the standard pad script:
movie skipped at 2000, title menu, NEW game at 3400, Reception): frame `M+k` of the loaded run
must equal frame `N+k` of a straight run. Snapshots every 30 frames, runs of 6600 frames:

| Scenario | Save N | Load M | Matching snapshots after the load | Mismatches |
|---|---|---|---|---|
| FMV playing (MDEC, CD streaming, XA audio) | 1500 | 1800 | 160 | 0 |
| Title screen | 2400 | 2700 | 130 | 0 |
| Main menu | 3000 | 3300 | 110 | 0 |
| CD load right after NEW game | 3450 | 3750 | 95 | 0 |
| Same state loaded 4 times | 2400 | 2700, 3000, 3300, 3330 | 109 | 0 |
| Tasks changed since the save (state: 8 tasks; at the load: 9, of which 3 new and 2 of the saved ones gone) | 2400 | 5010 | 53 | 0 |
| Tasks changed since the save (state: 7 tasks; at the load: 9) | 3450 | 5400 | 40 | 0 |
| Before the first frame (fiber never ran) | 0 | 600 | 200 | 0 |

The same eight scenarios under Wine (Windows build): identical results. Audio (`DCB_AUDIO_DUMP`)
of the FMV scenario after the load equals the straight run's from the save point, byte for byte
(6.5 MB). `DCB_STATE_STRESS=1` (save, run one frame, fingerprint CPU+RAM+VRAM, load, run it again,
compare) passed at all 12999 frame boundaries of a 13000-frame run through the name-entry screen,
and at 6499 boundaries under Wine; `n=7` and `n=97` passed too. A run recorded with `DCB_RECORD`
across a load replays (`DCB_REPLAY`) bit-identical to the straight run. The straight run itself
still matches the pre-change baseline (200/200 snapshots). Save takes about 3 ms (3.7 MB), load
about 5 ms including the safety copy (debug build).

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
  in-process save states sound. Concretely, for HLE code that calls guest code or polls (anything
  that can reach a frame boundary): no `std::vector`/`std::string`/smart pointer or other
  heap-owning local may be alive across that call, and no pointer or reference to a `Task` or
  `Fiber` object (they are recreated by a load; keep a cookie). Scope such locals so they die
  first, or use fixed-size stack arrays. See the [audit](#yield-hazard-audit).
- **Debugging:** gdb and Windows debuggers handle fibers. A backtrace taken on the host side
  doesn't show game frames; stop inside the game fiber (or use `DCB_WATCHDOG`) to see them.

## Roadmap

1. ~~Baseline frame-hash check on `main`~~: done; two baseline runs matched each other.
2. ~~Game on a fiber and the host loop; pause and frame-advance~~: done; 202/202 snapshot
   frames identical to `main`.
3. ~~Input record / replay~~: done (`DCB_RECORD`, `DCB_REPLAY`, `DCB_REPLAY_EXIT`); replays are
   bit-identical to the recorded run.
4. ~~Save states within a run~~: done (`F5`/`F6`/`F7`, `DCB_STATE_SAVE_AT`/`LOAD_AT`/`STRESS`);
   bit-identical after a load in every scenario tested, Linux and Windows (Wine). Follow-ups:
   check the Windows switch on real Windows; MSVC build (the switch as a MASM file); memory
   card contents in states (optional, off by default in most emulators).
5. ~~Trainer~~: done; GameShark codes from `cheats/<serial>.txt` applied at the frame boundary
   (`src/platform/trainer*.cpp`), memory search and the `F4` panel.
6. Custom Battle: approach A, then B.
7. Optional: save states that survive a restart — **verdict: not reasonably feasible**
   (investigated for the native-menu milestone, September 2026). The binary is PIE, so code,
   statics, heap and stacks all move every run. Game stacks at a frame boundary hold return
   addresses into our `.text`, pointers to long-lived heap objects (`Machine`/`Bios`/`Mmio`/
   `System`, the guest RAM buffer), pointers to statics, and main-thread stack addresses
   (measured with `tools/re/scan_stacks.py` on a real 3.7 MB state: 61 code + 45 heap + 42
   binary-data + 33 main-stack values in 4 KB of stacks). `MAP_FIXED_NOREPLACE` replays stack
   mappings exactly (verified), but that is the easy 10%: the heap objects, statics and code
   addresses would need fixing too (non-PIE build + fixed arenas for every long-lived object),
   and ASLR-disabled libc/SDL addresses inside `ucontext_t` plus C++ exception state would
   still break. The robust cross-session save is memory-card backup/restore (pause menu).
   Rewind, in-game settings menu, threaded rendering remain open.
