# The IPC

The IPC is the QL's second processor, an Intel 8049. It scans the keyboard, makes the sound and receives from the serial ports, and the 68008 talks to it a bit at a time through the ZX8302. The plan settled on emulating its command protocol rather than running an 8049 (docs/plan.md, The machine): QDOS, and software that drives the hardware directly, see the same replies a real IPC gives. Running a real 8049 from a supplied IPC ROM dump (`--ipc-rom`) is not done yet.

What each command does is read off a commented disassembly of the IPC ROM (`ipc8049.txt`, JV 970715, on [Dilwyn Jones's disassemblies page](https://sinclairql.net/djw/docs/disassem/index.html)). The command numbers agree with Minerva's `inc/ipcmd`, and the link's wiring is MAME's (`src/mame/sinclair/zx8302.cpp` and `ql.cpp`, BSD-3-Clause). Addresses below such as `$24D` are in the 8049's ROM.

## The link

From the 68008's side, as the JS ROM drives it (its routines at `$2F7C`, `$2F96` and `$2F9A`), each bit is one write to the ZX8302's link register and a look at its status register:

| | |
|---|---|
| `$18003`, write | `%11x0` sends the bit x; `%1110` listens for a bit from the IPC. Bit 0 is the start bit: a write with it set, such as the ROM's `$01` at power-on, only releases the line and sends nothing. |
| `$18020`, read | Bit 6 is busy; bit 7 is the bit on the line once the IPC has taken or given it. |

A command is four bits. Its parameters follow, then its reply, all most significant bit first, a byte as two nibbles with the high one first. The line is wired AND, so while listening the 68008 drives 1 and sees the IPC's bit.

**The IPC answers at once.** Busy never reads set, and a bit takes no emulated time. On a real QL the IPC takes about 25 µs a bit, and up to about 0.4 ms to notice a command, since it looks for one only once per pass of its main loop. Those figures are worked out from the disassembly's cycle counts, not measured, and nothing in Stage 2 depends on them.

## Commands

| | Command | Takes | Replies | Here |
|---|---|---|---|---|
| 0 | Reset | -- | -- | The IPC starts again from IPCinit: the keyboard buffer and every flag cleared. The QL's reset does the same. |
| 1 | Status | -- | a byte | Bit 0: a key buffered, or one held. Bit 2: SHIFT, CTRL or ALT changed. Bit 3: a key held. Bit 1 (sound playing), bits 4 and 5 (serial input waiting) and bit 7 (a serial timeout) are always 0 here. Bit 6 is the P2.6 pin, which nothing drives; MAME reads it as 0, and so does this. |
| 2-5 | Open and close serial channels | -- | -- | Taken and ignored. |
| 6, 7 | Read serial channel 1 or 2 | -- | a count, then that many bytes | `$00`: nothing received. |
| 8 | Read the keyboard | -- | a nibble, then a nibble and a byte for each key | See below. |
| 9 | KEYROW | a nibble | a byte | The matrix row selected by the nibble's low three bits, read as it is. |
| 10 | Sound | 8 bytes | -- | Taken and ignored until Stage 6. |
| 11 | Kill sound | -- | -- | Ignored. |
| 12 | P2.3 | a nibble | -- | Taken and ignored. On a real IPC a 0 holds a level-2 interrupt on for good. |
| 13 | Baud rate | a nibble | -- | Taken and ignored. |
| 14 | Random number | -- | 2 bytes, low first | As the IPC computes it (`$73A`), including its use of whatever the last keyboard scan left in register R4, and each typed key stirring it. |
| 15 | Test | a byte | the same byte | As the IPC does it. |

**Reading the keyboard**, command 8, replies with a nibble: bit 3 set if a key is held, bits 2-0 how many keys follow, up to 7. Each key is then a nibble -- bit 3 lost keys, bit 2 SHIFT, bit 1 CTRL, bit 0 ALT -- and its keycode. Reading empties the buffer.

## The keyboard

The machine presses and releases keys by name (`QLKey` in `core/src/ipc.h`), each numbered by its place in the matrix: row × 8 + column, where the row is the KEYROW row and the column its bit. The matrix is MAME's, which is the QL User Guide's KEYROW table. A key's keycode is `((7 - row) << 3) | column`: A, in row 4 at column 4, is `$1C`. SHIFT, CTRL and ALT, row 7's columns 0-2, are not keys of their own: they only appear in each key's modifier nibble, and in KEYROW's row 7.

The IPC scans its keyboard about 2,400 times a second, once every 3,150 68008 clocks; that is its idle loop of about 308 machine cycles at 733,333 a second. Each scan does what the IPC ROM's does (`$500`-`$5BE`):

- **Debounce.** A key must be the only one down for 6 scans running before it is buffered, about 2.5 ms. From the 7th scan on it is held, which status and command 8 report and QDOS uses for auto-repeat. A key is buffered once, however long it is held: auto-repeat is QDOS's job, not the IPC's.
- **No rollover.** A scan that finds two keys down forgets the key it was debouncing. Neither key is buffered, and held is cleared. When one is let go, the other is debounced from the start and buffered, even if it already was.
- **Modifiers.** A scan that finds SHIFT, CTRL or ALT changed does nothing else. From then until a scan finds no other key down, every key is ignored. So a modifier has to go down at least two scans before its key: one to see it change, one to see it alone. A key already down when a modifier changes is shut out until it is let go.
- **The buffer** holds 7 keys. An 8th is lost, and bit 3 of the 7th key's modifier nibble says so.

## Not done

- **Sound** (Stage 6) and **serial input**: their commands are accepted, with no effect.
- **CTRL-ALT-7.** A real IPC does not buffer it: it interrupts the 68008 at level 5 and starts itself again. Here it is buffered like any other key.
- **The IPC's interrupt lines.** It drives none here: neither CTRL-ALT-7's level 5 nor command 12's level 2.
- **Timing.** Busy never reads set, and the IPC keeps scanning while it answers a command, where a real one would stop. Its wait at power-on for the line to be released is not modelled.
- **A real 8049**, run from a supplied IPC ROM dump.
