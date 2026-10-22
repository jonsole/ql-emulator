# The machine

The QL around its 68008: what the CPU sees when it reads and writes, what the display draws, and how the machine is run. It grows a section per part as the peripheral chip (ZX8302) and the IPC are added.

## Memory map

The QL's 68008 is the 48-pin version, which drives only address lines A0-A19: its address space is 1 MB, and the CPU's 24-bit addresses wrap there. An access to `$130000` is an access to `$030000`.

| Addresses | What | Emulated |
|---|---|---|
| `$00000`-`$0BFFF` | The system ROM, 48K | Yes. Writes are ignored. An image under 48K leaves the rest reading as 0; one over 48K is refused. |
| `$0C000`-`$0FFFF` | The ROM cartridge port, 16K | Not yet: reads as 0. |
| `$18000`-`$1BFFF` | I/O: the ZX8302 at `$18000`, the ZX8301's control register at `$18063` | The ZX8301's control register (write-only; see [The display](#the-display)). The rest not yet: reads as 0, writes ignored. |
| `$20000`-`$3FFFF` | RAM, 128K; the screen at its foot | Yes. |
| `$40000`-`$BFFFF` | The 512K RAM expansion area | Yes, with `--ram 640`. Otherwise reads as 0. |
| `$C0000`-`$FFFFF` | Expansion ROMs | Not emulated: reads as 0. |

Any read that nothing answers gives 0, and any write that nothing takes is ignored -- MAME's choice in its QL driver. The map is that driver's (`src/mame/sinclair/ql.cpp`, BSD-3-Clause), whose notes call `$40000` onwards "the official 512K expansion ram area".

RAM comes in the two sizes the plan offers: the standard 128K, and 640K with the expansion area filled.

## The display

The ZX8301 draws the screen from RAM onto a 512×256 canvas, in either mode, at 50 frames a second. There is no hardware border. The encoding is MAME's (`src/mame/sinclair/zx8301.cpp`, BSD-3-Clause) and sQLux's (`src/SDL2screen.c`, Zlib), which agree on it.

**The control register** at `$18063` is write-only:

| Bit | Set means |
|---|---|
| 1 | The display is off: the screen is black. |
| 3 | Mode 8; clear for mode 4. |
| 7 | The second screen, at `$28000`; clear for the first, at `$20000`. |

**Screen memory** is 256 lines of 128 bytes, 64 words a line, from the screen's base.

- **Mode 4** (512×256, four colours): each word is 8 pixels. The even byte holds each pixel's green bit and the odd byte its red, bit 7 leftmost: black, red, green, white.
- **Mode 8** (256×256, eight colours): each word is 4 pixels, each drawn two dots wide. The even byte holds green and flash in bit pairs from bit 7, the odd byte red and blue. The colour is green × 4 + red × 2 + blue: black, blue, red, magenta, green, cyan, yellow, white.

**Flash, in mode 8.** A pixel with its flash bit set starts flashing in its own colour, and the next pixel with the bit set ends it, after itself. In the flash's on phase, the pixels in between -- and the one that ends it -- show the starting colour; in the off phase they show their own. Flashing never carries over to the next line. The phase changes every 32 frames from power-on, off first: sQLux's frame counter, whose bit 5 is the phase. MAME toggles at 2 Hz from a timer its own notes mark for removal; a divider on the frame count is the likelier hardware.

**Not yet:** contention, the ZX8301 holding the CPU off the bus while it reads the screen.

## Running

The machine runs at 7.5 MHz in frames of 150,000 clocks, 50 a second. Frames are counted from power-on: frame k ends at k × 150,000 clocks, and running a number of frames runs whole instructions until the end of the last of them, so an instruction that runs over a frame's end does not move the frames after it.

Power-on and the reset button both run the 68008's reset sequence, which reads the stack pointer and the PC from the ROM's first eight bytes.
