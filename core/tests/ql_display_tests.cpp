// The QL's screen, as the ZX8301 draws it from screen memory onto the
// 512x256 canvas.
//
// The encoding is MAME's ZX8301 (src/mame/sinclair/zx8301.cpp, BSD-3-Clause)
// and sQLux's (src/SDL2screen.c, Zlib), which agree on it:
// - 128 bytes a line, 256 lines, from $20000 -- or $28000 with the control
//   register's bit 7 set.
// - Mode 4: each word is 8 pixels; the even byte holds green, the odd red,
//   bit 7 leftmost. Black, red, green, white.
// - Mode 8 (control bit 3): each word is 4 pixels, each drawn two dots wide;
//   the even byte holds green and flash, the odd red and blue, in bit pairs
//   from bit 7. Colour = green x 4 + red x 2 + blue: black, blue, red,
//   magenta, green, cyan, yellow, white.
// Expected pixels below are worked out from that by hand.

#include "ql.h"
#include "test_main.h"
#include "test_rom.h"

#include <cstdint>

using qltest::rom_running;
using qltest::STOP;
using qltest::SUPERVISOR;

namespace {

constexpr uint32_t SCREEN = 0x020000;

// The colours as MAME's palette gives them.
constexpr uint32_t BLACK = 0x000000;
constexpr uint32_t BLUE = 0x0000FF;
constexpr uint32_t RED = 0xFF0000;
constexpr uint32_t MAGENTA = 0xFF00FF;
constexpr uint32_t GREEN = 0x00FF00;
constexpr uint32_t CYAN = 0x00FFFF;
constexpr uint32_t YELLOW = 0xFFFF00;
constexpr uint32_t WHITE = 0xFFFFFF;

uint32_t pixel(ql::QL& machine, unsigned x, unsigned y) {
    return machine.screen()[y * ql::SCREEN_WIDTH + x];
}

} // namespace

// Mode 4, the first word: green $AC = 1010 1100, red $CA = 1100 1010. Pixel by
// pixel (green, red): (1,1) white, (0,1) red, (1,0) green, (0,0) black, (1,1)
// white, (1,0) green, (0,1) red, (0,0) black. The next word and the next line
// are untouched: black.
TEST(mode_4_draws_green_and_red_bits_as_four_colours) {
    ql::QL machine(rom_running({STOP, SUPERVISOR}), 128);
    machine.poke(SCREEN, 0xAC);
    machine.poke(SCREEN + 1, 0xCA);
    const uint32_t expected[8] = {WHITE, RED, GREEN, BLACK, WHITE, GREEN, RED, BLACK};
    for (unsigned x = 0; x < 8; x++) CHECK_EQ(pixel(machine, x, 0), expected[x]);
    CHECK_EQ(pixel(machine, 8, 0), BLACK);
    CHECK_EQ(pixel(machine, 0, 1), BLACK);
}

// Mode 8, set by writing $08 to the control register at $18063, with all eight
// colours in two words. The even byte's pairs are green and flash, the odd
// byte's red and blue, from bit 7:
//   word 1: even $8A = 10 00 10 10, odd $D9 = 11 01 10 01:
//           (G,R,B) 111 white, 001 blue, 110 yellow, 101 cyan
//   word 2: even $02 = 00 00 00 10, odd $2C = 00 10 11 00:
//           000 black, 010 red, 011 magenta, 100 green
// each pixel two dots wide.
TEST(mode_8_draws_eight_colours_two_dots_wide) {
    ql::QL machine(rom_running({
                       0x13FC, 0x0008, 0x0001, 0x8063,   // MOVE.B #$08,($00018063).L
                       STOP, SUPERVISOR,
                   }),
                   128);
    machine.reset();
    machine.run_frames(1);
    machine.poke(SCREEN, 0x8A);
    machine.poke(SCREEN + 1, 0xD9);
    machine.poke(SCREEN + 2, 0x02);
    machine.poke(SCREEN + 3, 0x2C);
    const uint32_t expected[8] = {WHITE, BLUE, YELLOW, CYAN, BLACK, RED, MAGENTA, GREEN};
    for (unsigned p = 0; p < 8; p++) {
        CHECK_EQ(pixel(machine, 2 * p, 0), expected[p]);
        CHECK_EQ(pixel(machine, 2 * p + 1, 0), expected[p]);
    }
}

// The control register's bit 7 shows the second screen, at $28000: with red
// at $20000 and white at $28000, the first pixel is white. Bit 1 turns the
// display off: the screen is black whatever RAM holds.
TEST(control_register_picks_the_screen_and_turns_it_off) {
    ql::QL second(rom_running({
                      0x13FC, 0x0080, 0x0001, 0x8063,   // MOVE.B #$80,($00018063).L
                      STOP, SUPERVISOR,
                  }),
                  128);
    second.reset();
    second.run_frames(1);
    second.poke(SCREEN + 1, 0x80);            // red at $20000
    second.poke(SCREEN + 0x8000, 0x80);       // white at $28000
    second.poke(SCREEN + 0x8001, 0x80);
    CHECK_EQ(pixel(second, 0, 0), WHITE);

    ql::QL off(rom_running({
                   0x13FC, 0x0002, 0x0001, 0x8063,   // MOVE.B #$02,($00018063).L
                   STOP, SUPERVISOR,
               }),
               128);
    off.reset();
    off.run_frames(1);
    off.poke(SCREEN, 0xFF);
    off.poke(SCREEN + 1, 0xFF);
    CHECK_EQ(pixel(off, 0, 0), BLACK);
    CHECK_EQ(pixel(off, 7, 0), BLACK);
}

// Mode 8's flash bit (MAME and sQLux agree): a pixel with it set starts
// flashing in its own colour, and the pixels after it, up to and including
// the next with the bit set, show that colour in the flash's on phase and
// their own in its off phase. Flashing ends at the end of a line. The phase
// changes every 32 frames from power-on, off first: sQLux's frame counter,
// whose bit 5 is the phase. (MAME uses a 2 Hz timer, marked as to be
// replaced.)
//   line 0, word 0: even $4B = 01 00 10 11, odd $93 = 10 01 00 11:
//     (G,F,R,B) red+flash, blue, green, white+flash
//   line 0, word 1: even $80, odd $80: yellow, then black
//   line 1, word 0: even $40, odd $80: red+flash, then black to the line's end
//   line 2, word 0: even $80, odd $40: cyan
TEST(mode_8_flash_shows_the_starting_colour_every_other_32_frames) {
    ql::QL machine(rom_running({
                       0x13FC, 0x0008, 0x0001, 0x8063,   // MOVE.B #$08,($00018063).L
                       STOP, SUPERVISOR,
                   }),
                   128);
    machine.poke(SCREEN, 0x4B);
    machine.poke(SCREEN + 1, 0x93);
    machine.poke(SCREEN + 2, 0x80);
    machine.poke(SCREEN + 3, 0x80);
    machine.poke(SCREEN + 0x80, 0x40);
    machine.poke(SCREEN + 0x81, 0x80);
    machine.poke(SCREEN + 0x100, 0x80);
    machine.poke(SCREEN + 0x101, 0x40);
    machine.reset();

    // Frame 1: the off phase, every pixel its own colour.
    machine.run_frames(1);
    const uint32_t off[5] = {RED, BLUE, GREEN, WHITE, YELLOW};
    for (unsigned p = 0; p < 5; p++) CHECK_EQ(pixel(machine, 2 * p, 0), off[p]);
    CHECK_EQ(pixel(machine, 500, 1), BLACK);
    CHECK_EQ(pixel(machine, 0, 2), CYAN);

    // Frame 32: the on phase. Line 0 is red from the first flash bit through
    // the second; line 1's flash runs to the line's end; line 2 starts afresh.
    machine.run_frames(31);
    const uint32_t on[5] = {RED, RED, RED, RED, YELLOW};
    for (unsigned p = 0; p < 5; p++) {
        CHECK_EQ(pixel(machine, 2 * p, 0), on[p]);
        CHECK_EQ(pixel(machine, 2 * p + 1, 0), on[p]);
    }
    CHECK_EQ(pixel(machine, 500, 1), RED);
    CHECK_EQ(pixel(machine, 0, 2), CYAN);

    // Frame 64: off again.
    machine.run_frames(32);
    CHECK_EQ(pixel(machine, 2, 0), BLUE);
}

RUN_TESTS()
