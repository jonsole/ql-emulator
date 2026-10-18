# Plan

Planned between 2026-10-16 and 2026-10-18. Everything below has been decided. A change of mind goes back through the same question and updates this file.

## What it is

- **A separate emulator in its own repository.** It is not a second core inside zx-spectrum-emulator's server.
- **Two goals, weighted equally:** writing and debugging your own 68k programs, and running and reverse engineering QL software.
- **No real QL is available to measure against.** Disputed behaviour is settled by the documentation, the ROM disassembly, and other emulators (sQLux, MAME and the MiSTer core).

## What carries over from zx-spectrum-emulator

- **Copied once, then left to drift apart.** No library is shared between the two repositories. The copy covers:
  - the Engine's job queue and run thread
  - the DAP and MCP front ends: their HTTP and JSON plumbing, and the shape of their tools
  - rewind's checkpoint-and-replay design
  - the screen and audio streams
  - the test harness (`test_main.h`)
- **Rewritten for the 68008:** everything that belongs to the Z80, meaning the registers, the disassembler, how calls and returns are tracked, the profiler's rules, and the debug info.
- **From the start**, addresses are 32-bit and the CPU core describes its own register set.
- **The same language and conventions:**
  - C++17 with MSVC and `/W4 /WX`, built with CMake behind `build.ps1`
  - an `AGENTS.md` carrying over the style, tests whose expected values are worked out rather than captured from the code, committing only when asked, and never using `git stash`

## Layout

| Path | What |
|---|---|
| `core/src/` | The CPU and machine parts (`m68008.cpp`, `zx8301.cpp`, `zx8302.cpp`, `ipc.cpp`, `microdrive.cpp`, `ql.cpp`), and the Engine, DAP and MCP code copied over |
| `core/tests/` | The test suite |
| `vscode-extension/` | Its own extension: debugger type `ql`, a screen panel, a 68k assembly language mode |
| `scripts/` | Fetching the test suites, `.mdv` tools |
| `docs/` | One file per topic |
| `tools/` | Downloaded vasm and vlink (gitignored) |
| `roms/` | ROM images (gitignored) |

The extension copies the parts of zx's extension that don't need VS Code and adapts them: the screen panel, the profile model and the assembly index.

## The server

- **The program:** `ql_server.exe`.
- **Options:** `--rom`, `--ram 128|640`, `--mdv1 <image>`, `--mdv2 <image>`, `--ipc-rom <dump>`, the port options, and `--no-audio`.
- **Default ports,** different from zx_server's so that both can run side by side:

  | | Default | Verify instance |
  |---|---|---|
  | DAP | 4712 | 14712 |
  | MCP | 8100 | 18100 |
  | Screen | 8600 | 18600 |
  | Audio | 8601 | — |

- **MCP tools keep zx_server's names wherever the meaning is the same:** `run`, `pause`, `step`, `get_screen`, `read_memory`, `set_breakpoint`, `set_watchpoint`, `key_down`, `profile`, `step_back`, and the others.
  - Tools that only make sense on the QL are added: `insert_mdv` and `load_binary`.
  - Tools that only make sense on the Spectrum, such as the tape and 128K paging tools, are not copied.
  - Addresses are written as six hex digits.

## The machine

- **The CPU** is the 68008 at 7.5 MHz. Our own core is clocked per bus access: each byte access is an event of 4 clocks plus wait states, and the ZX8301 can delay any of them. Prefetch and internal cycles are counted but not modelled at the pins. A word access is two byte cycles, and a long word is four.
- **Display contention** follows the model in MiSTer's `ql_timing.sv`, and the one sQLux fits to measurements taken on a real QL.
- **Hardware:** the 128K QL, plus RAM expansion up to 640K. Not emulated for now: the Gold Card, the Trump Card and floppy drives.
- **The IPC.** Its command protocol is emulated over the ZX8302's link register, so QDOS and games that drive the hardware directly both work. A real 8049 runs only when you supply the IPC ROM dump with `--ipc-rom`.
- **Microdrives** are emulated at the hardware level, from `.mdv` images in the QLay layout, so the ROM's own driver does the work.
- **The real-time clock** is set from the host clock at reset, and that value is recorded as a rewind input. After that it advances only in emulated time.
- **The screen canvas** is 512×256 in both modes, with mode 8's pixels drawn two wide, at 50 Hz. There is no hardware border.
- **Keys.** A PC key maps to the QL key of the same name: letters, digits, the cursor keys, F1–F5, ESC, TAB, ENTER and SPACE. The modifiers map to SHIFT, CTRL and ALT. A PC key whose symbol sits on a different key on the QL types that symbol, with the emulator pressing whatever the QL needs to produce it.
- **Speed.** Realtime at 7.5 MHz, with contention, is required in a Release build. About 10x realtime is the design target for scripted runs.

## Your own programs

- **The assembler** is vasm (`vasmm68k_mot`, Motorola syntax), run with `-Felf -dwarf` and then vlink. You download it yourself; it is never bundled, because its licence forbids that.
- **Source-level stepping** comes from the emulator reading the DWARF `.debug_line` table.
- **Loading.** A launch loads the binary straight into memory, either at an address you give or as a QDOS job with its data space set. A host folder mapped as a `win1_` device comes later.

## ROMs

- **Minerva** (GPL v2) ships with releases as a separate file, with a link to its source.
- **The JS ROM** is the compatibility reference. You supply it in `roms/`. Tests that need it skip when it's missing.
- **The IPC ROM dump** is treated the same way as JS.
- The Sinclair ROMs are never committed or bundled.

## Testing

- **The CPU gate** is SingleStepTests/m68000 (MIT).
  - It is fetched by script, not committed.
  - A fixed sample runs in the fast suite, and the full set is a slow test.
  - 68008 timings come from splitting each word access into two byte cycles. A few instructions are checked by hand against the 68008 tables in the M68000 User's Manual.
- **Expected values are worked out, never captured** from the code's own output.

## Stages

1. **The 68008 as a standalone library** with its own tests, passing the test sample. Nothing is copied from zx until it passes.
2. **SuperBASIC on screen.** The JS ROM boots to the prompt, with the screen in both modes and the keyboard working through the IPC. You can type and run a BASIC program, driven over MCP. The Engine, DAP and MCP code are copied in at this stage. *This is the first milestone.*
3. **The debugger:** registers, disassembly, breakpoints, step and step-over. A copy of the `zx-live-verify` skill for the QL.
4. **Your own programs:** vasm, DWARF, and loading straight into memory.
5. **Microdrive images.**
6. **IPC sound.**
7. **Rewind and the profiler** on the QL.

## QL disassemblies

- **The standard** is the same as the Spectrum ones: every byte reassembles exactly, every routine is described, every address is a label, and notes are kept as the work goes.
- **They live in a separate `ql-disassemblies` repository**, added here as a submodule. It is created when the first disassembly starts.
- **The tooling is our own Python pipeline**, also built when the first disassembly starts. A control file names the code, data, text and comments. The pipeline:
  - uses the core's disassembler;
  - emits vasm source and checks that it reassembles byte for byte;
  - renders HTML in the same style as the Spectrum disassemblies.

## Releases

Releases start no earlier than the first milestone and follow the same model as zx-spectrum-emulator:
- GitHub Releases, for Windows x64
- Minerva bundled
- one tag covering both the server and the extension
- a `CHANGELOG.md` with an **Unreleased** section from the first code commit
- a release cut only when asked

## Facts found (2026-10-16)

- **Sinclair QL ROMs** are tolerated in the community but not licensed. Amstrad's 1999 permission covers the Spectrum ROMs only. North American rights are said to rest with Frank Davis and Paul Holmgren. Sources: Dilwyn Jones's ROM page, https://www.sinclairql.net/djw/qlrom/index.html, and the COPYRIGHT file in uQLx and sQLux.
- **Minerva** is GPL v2, at https://github.com/SinclairQL/Minerva; the current version is 1.98j4. sQLux bundles it.
- **The IPC ROM.**
  - A dump exists: MAME's `ipc8049.ic24`, and `rtl/ipc8049.hex` in the MiSTer core. It has the same rights problem as the Sinclair ROMs.
  - MAME runs a real 8049, and sQLux added one as an option in 2026.
  - The link protocol is described in the comment at the top of sQLux's `ipc.c`. The command numbers are in Minerva's `inc/ipcmd`. There is also a disassembly of the IPC ROM at https://dilwyn.theqlforum.com/docs/disassem/index.html.
- **CPU tests.**
  - https://github.com/SingleStepTests/680x0 has no licence (its issue #1 is still open).
  - https://github.com/SingleStepTests/m68000 is MIT, generated from MAME's microcoded core. Its JSON holds the initial and final state, plus every bus transaction with its cycle count.
  - Neither covers the 68008.
- **vasm's licence** allows redistribution only unmodified, and use only for non-commercial purposes. It has no QDOS output format. The alternative is GNU `m68k-elf-as -g`. For C: xtc68 (c68, MIT) and qdos-gcc 2.95.3 (GPL v2).
- **`.mdv`, the QLay layout,** used by MAME, MiSTer and sQLux: 255 sectors of 686 bytes, 174,930 bytes in all. Gaps are rebuilt from timing. Q-emuLator uses a different, variable-length format.
- **Executables in zip files** lose their 64-byte QDOS header, which holds the data space size, unless the zip carries Info-ZIP extra field `0xfb4a`.
- **Display contention.** MiSTer's `rtl/ql_timing.sv` divides a line into 40 chunks of 12 cycles, and on visible lines the ZX8301 holds 32 of them. sQLux fits its model to measurements taken on a real QL. Code in internal RAM is often quoted as about 50% slower.
- **68k disassembly tools.** Nothing like SkoolKit exists. The nearest tools:
  - IRA with vasm (the Aira Force front end)
  - Oxore/m68k-disasm
  - Capstone
  - DISA, which runs on the QL itself
