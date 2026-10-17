# Plan

Planning began on 2026-10-16 and is still going. This file records three things: what has been decided, what is still open, and the facts that bear on both. An open question carries a recommendation, but it isn't decided until it moves into **Decided**.

## Decided

- **A separate emulator in its own repository.** The QL is not a second core inside zx-spectrum-emulator's server. Code and approaches can still be carried over from that project.
- **Two goals, weighted equally:** writing and debugging your own 68k programs, and running and reverse engineering QL software.
- **A 68008 core of our own**, rather than an existing one: Moira and Musashi were both considered and turned down.
- **No real QL is available to measure against.** Disputed behaviour is settled by the documentation, the ROM disassembly, and other emulators (sQLux, MAME and the MiSTer core).

## Open questions

Each one has a recommendation.

1. **What carries over from zx-spectrum-emulator's `cpp-core`?** It could be copied and adapted, or used only as a pattern: the DAP and MCP front ends, the Engine's job queue, rewind, the profiler, the VS Code extension. Rewind's design works for any machine, but its code is written around the Spectrum. The profiler's rules for when a call ends belong to the Z80. *Recommendation:* copy the shape of the Engine, DAP, MCP and rewind, and adapt them for the 68008. Write the CPU-specific rules (calls, returns, stack frames) from scratch.
2. **How finely is the CPU clocked?** *Recommendation:* per bus access. Each byte access is an event of 4 clocks plus wait states, and the ZX8301 can delay any one of them. Prefetch and internal cycles are counted, not modelled at the pins. Nothing on the QL needs finer timing than that.
3. **Which hardware?** *Recommendation:* the 128K QL plus RAM expansion up to 640K. The Gold Card, the Trump Card and floppy drives wait until something needs them.
4. **At what level is the IPC emulated?** *Recommendation:* its command protocol over the ZX8302's link register. That way QDOS and games that drive the hardware directly both work. A real 8049 core runs only when you supply the IPC ROM dump yourself.
5. **At what level are microdrives emulated?** *Recommendation:* the hardware, from `.mdv` images. The ROM's own driver does the work, so loaders and copy protection behave as they do on a real QL.
6. **How do your own programs get into the machine during development?** *Recommendation:* a launch that loads the assembled binary straight into memory, at an address or as a QDOS job with its data space set. A host folder mapped as a `win1_` device can come later.
7. **The real-time clock under rewind.** *Recommendation:* set it from the host clock once at reset, record that value as a rewind input, then advance it in emulated time only.
8. **QL disassemblies.** *Recommendation:* the same standard as the Spectrum ones in zx-spectrum-disassemblies. Every byte reassembles exactly, every routine is described, and every address is a label. SkoolKit doesn't handle the 68k, so the tooling has to be built.
9. **Speed.** *Recommendation:* realtime at 7.5 MHz with contention is required, and about 10x realtime is the design target for scripted runs.
10. **Stages.** *Recommendation:*
    1. The 68008 core, passing its tests.
    2. The JS ROM boots to SuperBASIC, with the screen in both modes and the keyboard working through the IPC. This is the first milestone.
    3. The debugger.
    4. Your own programs, with an assembler and source-level stepping.
    5. Microdrive images.
    6. IPC sound.
    7. Rewind and the profiler.
11. **Assembler and debug info.** *Recommendation:* vasm (`vasmm68k_mot`, Motorola syntax), using `-Felf -dwarf` and vlink. The emulator reads the DWARF `.debug_line` table. vasm is never bundled: you download it yourself.
12. **ROMs.** *Recommendation:*
    - Minerva ships as a separate GPL v2 file, with a link to its source.
    - JS is the compatibility reference. You supply it, and tests that need it skip when it's missing.
    - The IPC dump is treated the same way as JS.
13. **The CPU test gate.** *Recommendation:* the MIT-licensed SingleStepTests/m68000 suite.
    - It is fetched by script, not committed.
    - A fixed sample runs in the fast suite, and the full set is a slow test.
    - 68008 timings come from splitting each word access into two byte cycles. A few instructions are checked against the 68008 tables in the M68000 User's Manual.

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
