// The QL machine: the memory map the 68008 sees, and running a ROM headless.
// Each test runs a few instructions of hand-assembled ROM; expected values
// come from what each program is defined to do.
//
// The map is MAME's QL driver's (src/mame/sinclair/ql.cpp, BSD-3-Clause):
// ROM at $00000-$0BFFF, the cartridge port to $0FFFF, the I/O area at $18000,
// RAM from $20000 -- 128K, or 640K with the expansion area from $40000 --
// and 0 from any read nothing answers.

#include "ql.h"
#include "test_main.h"
#include "test_rom.h"

#include <cstdint>
#include <stdexcept>
#include <vector>

using qltest::rom_running;
using qltest::STOP;
using qltest::SUPERVISOR;

namespace {

constexpr uint32_t INITIAL_SSP = qltest::ROM_INITIAL_SSP;

} // namespace

// From reset the program runs: MOVEQ #$2A,D0 then MOVE.L D0,($30000).L puts
// $0000002A in RAM at $30000.
TEST(a_rom_runs_from_reset_and_writes_to_ram) {
    ql::QL machine(rom_running({
                       0x702A,                   // MOVEQ #$2A,D0
                       0x23C0, 0x0003, 0x0000,   // MOVE.L D0,($00030000).L
                       STOP, SUPERVISOR,
                   }),
                   128);
    machine.reset();
    machine.run_frames(1);
    CHECK_EQ(machine.peek(0x030000), uint8_t{0x00});
    CHECK_EQ(machine.peek(0x030001), uint8_t{0x00});
    CHECK_EQ(machine.peek(0x030002), uint8_t{0x00});
    CHECK_EQ(machine.peek(0x030003), uint8_t{0x2A});
    CHECK_EQ(machine.cpu().registers().ssp, INITIAL_SSP);
}

// The ROM cannot be written: a byte stored over ROM's $A5 at $200 reads back
// as $A5. Nothing answers between the cartridge port and the I/O area: a byte
// read from $14000 is 0, overwriting the $FF put in RAM first.
TEST(rom_is_read_only_and_nothing_answers_reads_as_zero) {
    std::vector<uint8_t> rom = rom_running({
        0x11FC, 0x0055, 0x0200,                   // MOVE.B #$55,($0200).W
        0x13F8, 0x0200, 0x0003, 0x0000,           // MOVE.B ($0200).W,($00030000).L
        0x13FC, 0x00FF, 0x0003, 0x0001,           // MOVE.B #$FF,($00030001).L
        0x13F9, 0x0001, 0x4000, 0x0003, 0x0001,   // MOVE.B ($00014000).L,($00030001).L
        STOP, SUPERVISOR,
    });
    rom.resize(0x400, 0);
    rom[0x200] = 0xA5;
    ql::QL machine(rom, 128);
    machine.reset();
    machine.run_frames(1);
    CHECK_EQ(machine.peek(0x000200), uint8_t{0xA5});
    CHECK_EQ(machine.peek(0x030000), uint8_t{0xA5});
    CHECK_EQ(machine.peek(0x030001), uint8_t{0x00});
}

// The QL's 48-pin 68008 drives only A0-A19, so its address space is 1 MB and
// repeats above that: a store to $130000 lands at $30000, and a read from
// $F00200 is ROM's $200.
TEST(addresses_wrap_at_one_megabyte) {
    std::vector<uint8_t> rom = rom_running({
        0x702A,                                   // MOVEQ #$2A,D0
        0x13C0, 0x0013, 0x0000,                   // MOVE.B D0,($00130000).L
        0x13F9, 0x00F0, 0x0200, 0x0003, 0x0001,   // MOVE.B ($00F00200).L,($00030001).L
        STOP, SUPERVISOR,
    });
    rom.resize(0x400, 0);
    rom[0x200] = 0xA5;
    ql::QL machine(rom, 128);
    machine.reset();
    machine.run_frames(1);
    CHECK_EQ(machine.peek(0x030000), uint8_t{0x2A});
    CHECK_EQ(machine.peek(0x030001), uint8_t{0xA5});
}

// 640K is the 128K QL with the 512K expansion area, $40000-$BFFFF, filled.
// The same stores to $60000, $BFFFF (the last expansion byte) and $C0000 (past
// it) leave $2A in the first two with 640K, and nothing anywhere with 128K.
// The plan offers those two sizes only.
TEST(ram_expansion_fills_40000_to_bffff) {
    std::vector<uint8_t> rom = rom_running({
        0x702A,                   // MOVEQ #$2A,D0
        0x13C0, 0x0006, 0x0000,   // MOVE.B D0,($00060000).L
        0x13C0, 0x000B, 0xFFFF,   // MOVE.B D0,($000BFFFF).L
        0x13C0, 0x000C, 0x0000,   // MOVE.B D0,($000C0000).L
        STOP, SUPERVISOR,
    });
    ql::QL expanded(rom, 640);
    expanded.reset();
    expanded.run_frames(1);
    CHECK_EQ(expanded.peek(0x060000), uint8_t{0x2A});
    CHECK_EQ(expanded.peek(0x0BFFFF), uint8_t{0x2A});
    CHECK_EQ(expanded.peek(0x0C0000), uint8_t{0x00});

    ql::QL standard(rom, 128);
    standard.reset();
    standard.run_frames(1);
    CHECK_EQ(standard.peek(0x060000), uint8_t{0x00});
    CHECK_EQ(standard.peek(0x0BFFFF), uint8_t{0x00});

    bool refused = false;
    try {
        ql::QL odd(rom, 256);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    CHECK(refused);
}

// The system ROM is 48K, $00000-$0BFFF; a larger image cannot be a QL system
// ROM, and is refused rather than cut short. A smaller one -- a test's -- has
// the rest of the space read as 0.
TEST(a_rom_image_over_48k_is_refused) {
    bool refused = false;
    try {
        ql::QL machine(std::vector<uint8_t>(0xC001, 0), 128);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    CHECK(refused);
    ql::QL machine(std::vector<uint8_t>(0xC000, 0x11), 128);
    CHECK_EQ(machine.peek(0x00BFFF), uint8_t{0x11});
}

// Frames are counted from power-on: frame k ends at k x 150,000 clocks (7.5
// MHz, 50 Hz), and run_frames runs whole instructions until the end of the
// frame asked for. With BRA.S * (18(4/0), User's Manual Table 7-10) after
// the reset's 64(12/0) (Table 7-15), the clock is 64 + 18k: 2 frames end at
// the first such value from 300,000, 300,016; one more frame at the first
// from 450,000, 450,010.
TEST(frames_end_at_multiples_of_150000_clocks) {
    ql::QL machine(rom_running({0x60FE}), 128);   // BRA.S *
    machine.reset();
    CHECK_EQ(machine.cpu().clock(), uint64_t{64});
    machine.run_frames(2);
    CHECK_EQ(machine.cpu().clock(), uint64_t{300'016});
    machine.run_frames(1);
    CHECK_EQ(machine.cpu().clock(), uint64_t{450'010});
}

RUN_TESTS()
