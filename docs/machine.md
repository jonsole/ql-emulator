# The machine

The QL around its 68008: what the CPU sees when it reads and writes, and how the machine is run. It grows a section per part as the display (ZX8301), the peripheral chip (ZX8302) and the IPC are added.

## Memory map

The QL's 68008 is the 48-pin version, which drives only address lines A0-A19: its address space is 1 MB, and the CPU's 24-bit addresses wrap there. An access to `$130000` is an access to `$030000`.

| Addresses | What | Emulated |
|---|---|---|
| `$00000`-`$0BFFF` | The system ROM, 48K | Yes. Writes are ignored. An image under 48K leaves the rest reading as 0; one over 48K is refused. |
| `$0C000`-`$0FFFF` | The ROM cartridge port, 16K | Not yet: reads as 0. |
| `$18000`-`$1BFFF` | I/O: the ZX8302 at `$18000`, the ZX8301's control register at `$18063` | Not yet: reads as 0, writes ignored. |
| `$20000`-`$3FFFF` | RAM, 128K; the screen at its foot | Yes. |
| `$40000`-`$BFFFF` | The 512K RAM expansion area | Yes, with `--ram 640`. Otherwise reads as 0. |
| `$C0000`-`$FFFFF` | Expansion ROMs | Not emulated: reads as 0. |

Any read that nothing answers gives 0, and any write that nothing takes is ignored -- MAME's choice in its QL driver. The map is that driver's (`src/mame/sinclair/ql.cpp`, BSD-3-Clause), whose notes call `$40000` onwards "the official 512K expansion ram area".

RAM comes in the two sizes the plan offers: the standard 128K, and 640K with the expansion area filled.

## Running

The machine runs at 7.5 MHz in frames of 150,000 clocks, 50 a second. Frames are counted from power-on: frame k ends at k × 150,000 clocks, and running a number of frames runs whole instructions until the end of the last of them, so an instruction that runs over a frame's end does not move the frames after it.

Power-on and the reset button both run the 68008's reset sequence, which reads the stack pointer and the PC from the ROM's first eight bytes.
