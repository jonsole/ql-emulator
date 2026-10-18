# ql-emulator

An emulator for the Sinclair QL. It has two jobs:

- **Writing and debugging 68k programs**, with source-level stepping, a profiler and rewind, driven from VS Code over DAP and from tools over MCP.
- **Running and reverse engineering QL software** from microdrive images.

It is a sister project to [zx-spectrum-emulator](https://github.com/jonsole/zx-spectrum-emulator) and follows the same approach: a cycle-counted CPU core of its own, with debugging built in from the start rather than added on.

## Status

**Stage 1 of 7: the 68008 core.** [docs/plan.md](docs/plan.md) sets out the design, the stages and the research behind them. The core runs its first instructions against SingleStepTests; nothing else exists yet.

## The machine

- A Motorola 68008 at 7.5 MHz, with an 8-bit data bus and a 1 MB address space.
- The ZX8301, which drives the display (512×256 in four colours, or 256×256 in eight) and takes the bus from the CPU while it does.
- The ZX8302: interrupts, the real-time clock, the microdrives, the network and serial transmit.
- An Intel 8049, the IPC: keyboard, sound and serial receive.
- 128K of RAM as standard, expandable.

## ROMs

The Sinclair QL ROMs (JM, JS and the rest) have never been formally licensed for distribution, so this repository does not include them. [Minerva](https://github.com/SinclairQL/Minerva), a replacement QL ROM, is GPL v2 and can be supplied alongside the emulator. ROM images go in `roms/`, which git ignores.

## Licence

MIT -- see [LICENSE](LICENSE).
