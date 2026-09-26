# HLE layer

Native code that replaces what the PS1 kernel and hardware did. No BIOS image is loaded at runtime.

| Dir      | Replaces                        | Level it hooks at |
|----------|---------------------------------|-------------------|
| `bios/`  | Kernel A0/B0/C0 tables, syscalls | Function number in `$t1` at jumps to 0xA0/0xB0/0xC0 |
| `gpu/`   | libgpu / libgs + GPU            | **GP0 packets.** Games build primitives in RAM with inline macros; `DrawOTag` hands over an ordering table of GP0 packets, so interpret those packets |
| `cdrom/` | libcd / libds                   | **Library calls** (`CdRead`, `CdSearchFile`, `CdControl`): map them onto files from `extracted/` |
| `spu/`   | libspu / libsnd                 | Start at the SPU register level (voices, ADSR, reverb); raise it to library level only where that's simpler |
| `pad/`   | libpad / libetc pad             | Library calls → `platform::Platform::pad_buttons` |
| `mcrd/`  | libmcrd / memory card BIOS      | Library calls → host save files |
| `hw/`    | 0x1F801xxx registers            | Fallback for code that pokes hardware directly (DMA, IRQ, timers) |

Rule: identify a library function (Ghidra's Psy-Q signatures), then replace it with an
**override** registered at its guest address, so the recompiled body never runs. Anything the game
does below library level lands in `hw/`, where it gets logged until it's handled.

GTE (COP2) is **not** here. Its opcodes are inlined into game code by libgte macros, so it lives in
`src/runtime` as an instruction-level implementation that generated code calls directly.

Kernel patches: `libpad`/`libetc` copy small code templates into BIOS kernel RAM (below
0x80010000) to hook the exception handler. The recompiler finds these templates (e.g.
`f_8006B504` jumps to 0xA000DFAC) but they never run here. The native replacements for the
functions that install them skip that step.
