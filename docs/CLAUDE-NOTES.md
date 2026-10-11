# Reconstructing `ZATACKA.EXE`

> [!NOTE]
> This document was written by Claude Code (Opus 5.5).

`zatacka.cpp` is a hand-written reconstruction of the game code of ZATACKA.EXE ("Achtung, die Kurve!", 1991). Compiled with Borland C++ 3.1, it reproduces the original executable **byte for byte**: all 47,390 bytes, including the C0 startup code, runtime library, BGI graphics library and floating-point emulator that the linker pulls in. This document collects what was important, interesting or difficult about getting there.

## The binary in brief

- Plain MZ executable, 47,390 bytes. The game code proper is tiny: CS:02C2..CS:12B5, about 4 KB (keyboard ISR, five helpers, `main`, and `random()` emitted out of line). Everything else is Borland's C0 startup, runtime library (RTL), `GRAPHICS.LIB` (BGI) and the 8087 emulator.
- Graphics via BGI: `initgraph` with VGA/VGAHI (640x480, 16 colors, mode 12h). The driver `EGAVGA.BGI` and the stroked font `TSCR.CHR` (Triplex Script, font 7) are loaded at runtime from the current directory. They are not linked in, which is why the build output includes them.
- Floating point goes through Borland's emulator: the code contains `INT 34h`..`INT 3Dh` stubs instead of x87 opcodes. The stubs patch themselves into real FPU instructions on first execution if an FPU is present.
- Input: INT 9 is hooked to read scancodes from port 60h directly (five keyboard players). The sixth player uses the mouse via INT 33h function 3.
- `rand()` is never seeded (`srand` is never called), so every run of the original is deterministic. That property made it possible to record golden traces of real games and replay them against a model and the reconstruction.

## Compiler and flags

```
BCC -ms -O2 -vi- -B ZATACKA.CPP GRAPHICS.LIB
```

The flags weren't guessed but read off the generated code:

| Flag | Evidence in the binary |
|---|---|
| Borland C++ **3.1** | The C0 startup, `GRAPHICS.LIB` and emulator code match only 3.1. BC++ 3.0 builds the same source, but its C0 scans the environment differently, its `GRAPHICS.LIB` is 0x3C5A bytes instead of 0x3C80, it puts C++ globals in `_DATA` instead of `_BSS`, and it multiplies by 4 with `shl, shl` instead of `mov cl,2 / shl`. BC++ 2.0 is ruled out because it has no `TSCR.CHR` (font 7 needs the 3.x graphics library). The RTL contains the string "Borland C++ - Copyright 1991 Borland Intl.". |
| `-ms` (small model) | The game calls the RTL (`delay`, `itoa`, `printf`, `malloc`, `sin`, `cos`) with near calls; only the BGI functions are far, because `graphics.h` declares them far. An early analysis note said "medium model", which was wrong. |
| C++, not C | `random()` is a real function at CS:1296, right after `main`. In C, `random` is a macro in `stdlib.h`; in C++ it is an `inline int random(int)`. |
| `-vi-` | That inline function was compiled out of line, so inline expansion was off. |
| `-O2` | Register reuse, no stack frame in functions without arguments or locals, the round-over loop's win threshold `score_to_win(participating_count) - 1` hoisted out of the loop (the compiler folds it to `participating_count*10 - 11`, computed once), and a strength-reduced loop in the death/scoring code (SI = 2j, DI = 80j). |
| `-B` | The module was compiled via TASM rather than BCC's built-in assembler. Single-pass TASM leaves the sequence `jg $+5 / jmp short / nop` for one forward branch in `main` (the solid/hole branch). The built-in assembler would emit a plain `jle` there. |

The compiler-version investigation went in stages. A BC++ 3.0 build came first: it played pixel-identically and six of the eight functions matched instruction for instruction. Moving to 3.1, plus the source details below, made it byte-identical.

## Source details forced by the generated code

With the right compiler, a lot of C still compiles to *almost* the right bytes. The remaining differences pinned down surprisingly specific source choices:

- **Inline asm for the overscan color in `main`.** `_AX = 0x1001; _BH = 14; geninterrupt(0x10);` compiles differently. More importantly, inline asm anywhere in a function turns off BCC's global register allocation for that function. The original `main` keeps its temporaries in stack slots, and only the `register int xi, yi` get SI/DI. So the asm block must stay asm, and the `register` keyword is load-bearing.
- **Inline asm for the PIC end-of-interrupt.** `outportb(0x20, 0x20)` loads the port into DX. The original uses the immediate-port form `out 20h, al`.
- **The case order in `keyboard_isr`.** The case bodies are laid out in the EXE in source order, and that order is not player order (Orange, Yellow, Green, Pink, Red after ESC and SPACE). Reordering the cases changes the bytes.
- **`px = prev_px = xi`, not `prev_px = px = xi`.** The store order in the spawn code follows the order of the chained assignment.
- **`++score[j]` as a function argument**, `participating_count <= 1` as the lobby exit condition, and separate `return 1`s in `crashes_when_dx_is_not_zero` (rather than one combined condition): each of these is visible in the instruction sequence.
- **24 unreferenced bytes of globals after `key_space` (DS:0EC2..0ED9).** The game code never touches them, but without them every later global shifts. Their original type and name are unknown; `int unused_0EC2[12]` preserves the layout.
- **Globals are declared in DGROUP address order** (DS:0EBE..0FC9), one per line with their address. BCC lays them out in declaration order, so the order is fixed.
- **Floating-point constants.** `10000.0` is stored as a float (DS:019C) and `0.035` as a double (DS:01A0). BCC stores exactly representable double constants as float, so the source most likely just wrote `10000.0`.

## What doesn't change the bytes

Learned while cleaning up the source, and useful to know when refactoring:

- `#define` constants and macros fold to the same code, including inside `asm` lines: BCC expands macros there, and C-style hex (`0x20`) works in asm.
- `volatile` makes no difference to BC++ 3.1's code generation. It is in the source now for correctness with a modern compiler: the key flags are written by the ISR and polled in `while (!key_space)` loops. It is unknown whether the original had it.
- Operand order of commutative additions (`MIN + random(R)` vs `random(R) + MIN`) and constant expressions like `CANVAS_WIDTH - 2 * SPAWN_MARGIN + 2` in place of `361` compile identically.
- Function and variable names don't appear in the EXE (no debug info). They do appear in the MAP file.
- BC++ 3.1 has no binary literals (`0b...`).

## Source structure (`zatacka.cpp`)

- **Header comment:** what it is, how addresses are written (`CS:xxxx`/`DS:xxxx` offsets, independent of load address), the exact BCC command line, and the runtime files needed.
- **`#define`s, grouped by topic:**
  - fonts and text sizes;
  - player IDs (`ID_RED`..`ID_BLUE`, five keyboard players plus the mouse player);
  - hole/solid timing;
  - spawn margins;
  - scoring;
  - head size (`THE_THICKNESS`, `TWO`);
  - screen geometry;
  - lobby layout;
  - turn rate;
  - hardware interfaces (keyboard interrupt and I/O port, PIC, video BIOS, mouse driver);
  - text strings;
  - scancodes (`KEYDOWN_x`, `KEYUP_x = KEYDOWN_x | 0x80`);
  - color roles (`CANVAS_COLOR`, `border_color(i)`, `player_color(id)`, `SCOREBOARD_COLOR`, `TEXT_COLOR`);
  - `is_obstacle`.
- **`palette[16][3]`:** the 6-bit VGA DAC values for BGI colors 0..15. Each row is annotated with the role macro that uses it, which connects the palette to the color roles.
- **Globals:** structure-of-arrays per player (`x`, `y`, `dir`, `px`, `py`, `score`, `dead`, …), each on its own line with its DS offset and meaning.
- **Functions, in original address order:**
  - `keyboard_isr` (CS:02C2): scancode → flag.
  - `draw_int_text` (CS:048A): draws a score.
  - `poll_mouse_buttons` (CS:04B8): INT 33h/3. `mouse_left = BX & ~RIGHT`, so the middle button counts as both.
  - `clear_screen_color` (CS:04CE).
  - `crashes_when_dx_is_not_zero` (CS:04F6): collision test of one column of the head.
  - `move_or_die` (CS:056F): accepts the new pixel position or kills the player and awards every survivor a point.
  - `main` (CS:06FC): setup, then labeled sections `label_splash`, the lobby, `label_round`, the main loop, `label_round_over`, `label_game_over`, `label_quit`. The `goto`s mirror the original control flow; restructuring into loops risks changing the code layout.

## Game mechanics worth knowing

- **Movement:** 1 px per tick, `x += sin(dir)`, `y += cos(dir)`. `dir = 0` points down and grows counter-clockwise. The start direction is `random(31415) / 10000.0`, in [0, π), so players never start heading west.
- **Pixel position:** `(int)x` truncates toward zero. Borland's float-to-int helper sets the x87 rounding control to chop and uses `FISTP`. A head at x = −0.9 therefore truncates to 0 and survives, making the left and top edges effectively about 1 px more lenient than the right and bottom.
- **Collision:**
  - The head is a 3x3 square, but only corner pixels are tested, and only those on the side the head moved towards. A diagonal step tests a single pixel.
  - `is_obstacle` is `getpixel > 7`, presumably meant as "any player color", but it also matches the scoreboard color and text color.
- **Holes:** with countdown C (100..299) and hole length h (7..11):
  - each hole is exactly h missing draws;
  - a cycle is C + 1 ticks, because the tick where the countdown hits 0 re-rolls but still runs the hole branch;
  - so a solid segment is C − h + 1 draws. Verified by debugging with C = 5, h = 2: 2 missing draws, then 4 drawn.
- **Spawn:** `x = 100 + random(361)`, `y = 100 + random(280)`. The x range is 2 wider than the symmetric one, for no apparent reason.
- **Original bugs, preserved:**
  - The initial hole timers are set for `cur_player` (always 0) inside a loop over `loop_i`, so only Red gets real timers. The others start in the hole branch with an uninitialized background buffer, which BGI draws as a single black pixel.
  - The overscan color is set as raw DAC index 14, which the game never programs (BGI color 14 is DAC 62). So the border shows the BIOS default for DAC 14, not the scoreboard gray.
- **Timing:** `delay(15 - alive_count / 2)` ms per tick, a busy-wait on PIT channel 0.

## The build pipeline

`build.py` runs the whole Borland toolchain in headless DOSBox; no DOS install is needed on the host.

1. **Floppy images → files.** The seven needed BC++ 3.1 installation disks (01, 02, 03, 07, 08, 09, 13 of the 3.5" 1.44 MB set) are FAT12 images. A ~20-line FAT12 reader in Python extracts the root directory of each. Only those seven disks hold anything the build needs:

   | Disk | Provides |
   |---|---|
   | 01 | `UNZIP.EXE`, `INCLUDE.ZIP` |
   | 02 | `BGI.ZIP` (`EGAVGA.BGI`, `TSCR.CHR`) |
   | 03, 13 | `CMDLINE.CA1`, `CMDLINE.CA2` (BCC) |
   | 07 | `TASM.CA1`, `XLIB.ZIP` (`GRAPHICS.LIB`) |
   | 08 | `SLIB.ZIP` (`C0S.OBJ`, `CS.LIB`, `MATHS.LIB`) |
   | 09 | `BIN.ZIP` (TLINK etc.), `TASM.CA2` |

2. **Unpacking inside DOSBox.** The installer's ZIPs use PKZIP 1.x "implode", which modern unzip tools mostly don't handle. So they are unpacked by Borland's own `UNZIP.EXE` from disk 1, running inside DOSBox. Split archives (`*.CA1` + `*.CA2`) are simply concatenated first. The unpacked installation is cached in `.cache/dosroot` (with its own `.gitignore`).
3. **Compiling.** BCC compiles and links via TASM. With `INCLUDE_MAP_AND_ASM=1`, a second compile with `-S` (without `-B`) also produces `ZATACKA.ASM`, and the linker's `ZATACKA.MAP` is copied out.
4. **Fixed file dates.** All files in the DOS tree get mtime 700000000 (1992-03-07), because `BCC -S` records source and header dates in `?debug` lines. Without this, the ASM listing would differ from run to run.
5. **Verification.** `ZATACKA.EXE`, `EGAVGA.BGI` and `TSCR.CHR` are compared byte for byte with `original-game/`. The build fails on any mismatch unless `ALLOW_MISMATCH=1`, which is meant for deliberate source modifications.

Details of the DOSBox invocation:
- Each step writes a `RUN.BAT` and a generated config: `output=surface` plus `SDL_VIDEODRIVER=dummy` for headless operation, `core=dynamic` and `cycles=max` for speed, no sound.
- `autoexec` mounts the root as `C:`, runs `call RUN.BAT`, then `exit`.
- DOS programs print to the emulated screen, not host stdout, so the batch file redirects to `C:\OUT.TXT`, which Python reads back as cp437.

The output directory is emptied before each build. It is only emptied if it contains nothing but files the build produces, so a wrong path (e.g. `.`) is refused rather than wiped.

## Docker

The `Dockerfile` wraps `build.py` for a fully pinned build:

- the Dockerfile frontend and the base image (`debian:trixie-20261005-slim`) are pinned by digest;
- apt is pointed at the `snapshot.debian.org` archives the base image itself names (trixie, trixie-updates, trixie-security as of 2026-10-05), so installed packages match the base image's archive state while apt still verifies signatures;
- `dosbox` and `python3.13` are pinned to exact versions, which documents them and makes a snapshot bump fail loudly until they are updated too;
- a `.dockerignore` sends only the inputs the build needs;
- the final stage is `FROM scratch` with only the output files, so `docker build -o <outdir> .` exports just those;
- two `--no-cache` builds produce identical output.

One caveat: Docker's local exporter only adds or overwrites files in `<outdir>` and never removes stale ones, so use a dedicated output directory.

## Difficulties and lessons

- **Most of the work was in the last few bytes.** A source that plays identically, and even one where most functions match, is far from byte-identical. Getting from "plays the same" to "same bytes" required the exact compiler release, the exact flags (including the non-obvious `-B`), and a dozen source-level choices that only show up as instruction order or register use.
- **Inline asm has non-local effects** in BC++: it disables global register allocation for the whole function. That one fact explained why `main` used stack slots for its temporaries.
- **Tooling archaeology.** Implode-compressed ZIPs, split cabinet-style archives and FAT12 images all had to be handled without the original installer. Running Borland's own `UNZIP.EXE` in DOSBox was simpler than finding a modern implementation of implode.
- **Determinism everywhere.** The original is deterministic (unseeded RNG), which enabled golden-trace testing. The build had to be made deterministic too: fixed DOS file dates, pinned Docker base image and packages, pinned DOSBox config, and a clean output directory.
- **Readability vs. byte identity.** The human-led cleanup phase replaced magic numbers with named constants and macros, renamed things after what they actually do (`move_or_die`, `holecycle_countdown`, hole/solid instead of gap/trail), and documented original bugs and quirks in comments. All of this was checked against the byte-identical build after every change. Each clarifying comment was also checked against the actual behavior, which corrected several earlier misreadings:
  - hole lengths are exact, and the off-by-one is in the cycle;
  - only corners are tested for collisions;
  - DAC 14 is not the scoreboard color.
