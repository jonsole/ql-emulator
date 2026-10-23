// The ZX8302's frame interrupt, interrupt register and real-time clock, as a
// QL program sees them. Each test runs a small hand-assembled ROM.
//
// From MAME's ZX8302 (src/mame/sinclair/zx8302.cpp and .h, BSD-3-Clause):
// - the interrupt register at $18021 reads as the pending interrupts -- the
//   frame interrupt is bit 3, $08 -- and a write clears the pending bits set
//   in its low five bits and sets the mask from its top three;
// - while anything is pending the ZX8302 holds IPL1L, which on the QL's
//   48-pin 68008 alone is interrupt level 2: autovector 26, at $68 (User's
//   Manual 6.3.2);
// - the real-time clock is four bytes at $18000-$18003, most significant
//   first, counting seconds.

#include "ql.h"
#include "test_main.h"
#include "test_rom.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

using qltest::rom_running;

namespace {

constexpr uint32_t COUNTER = 0x030000;
constexpr uint32_t HANDLER = 0x000200;

/// Puts `words` into the ROM at `at`.
void put(std::vector<uint8_t>& rom, uint32_t at, std::initializer_list<uint16_t> words) {
    if (rom.size() < at + 2 * words.size()) rom.resize(at + 2 * words.size(), 0);
    for (uint16_t w : words) {
        rom[at] = static_cast<uint8_t>(w >> 8);
        rom[at + 1] = static_cast<uint8_t>(w);
        at += 2;
    }
}

/// A ROM whose main program lowers the interrupt mask to 0 and waits, with
/// level 2's autovector pointing at `handler` at HANDLER.
std::vector<uint8_t> rom_with_level_2_handler(std::initializer_list<uint16_t> handler) {
    std::vector<uint8_t> rom = rom_running({
        0x46FC, 0x2000,   // MOVE #$2000,SR -- supervisor, mask 0
        0x60FE,           // BRA.S *
    });
    put(rom, 26 * 4, {uint16_t(HANDLER >> 16), uint16_t(HANDLER)});
    put(rom, HANDLER, handler);
    return rom;
}

uint32_t long_at(const ql::QL& machine, uint32_t address) {
    return uint32_t(machine.peek(address)) << 24 | uint32_t(machine.peek(address + 1)) << 16
           | uint32_t(machine.peek(address + 2)) << 8 | machine.peek(address + 3);
}

} // namespace

// Frames run from one vertical sync to the next, the frame interrupt raised as
// each begins. A handler that counts and acknowledges ($08 to $18021) is
// entered once a frame: 5 frames from power-on, 5 interrupts -- the first,
// raised during reset with the mask at 7, taken once the program lowers it.
TEST(the_frame_interrupt_comes_once_a_frame_at_level_2) {
    ql::QL machine(rom_with_level_2_handler({
                       0x52B9, 0x0003, 0x0000,           // ADDQ.L #1,($00030000).L
                       0x13FC, 0x0008, 0x0001, 0x8021,   // MOVE.B #$08,($00018021).L
                       0x4E73,                           // RTE
                   }),
                   128);
    machine.reset();
    machine.run_frames(5);
    CHECK_EQ(long_at(machine, COUNTER), 5u);
}

// Until it is acknowledged the frame interrupt stays pending, IPL1L held: a
// handler that does not acknowledge it is entered again as soon as it
// returns, far more than once a frame. And with the mask left at 7, the
// interrupt register reads $08 -- the frame interrupt pending since power-on
// -- and, once $08 is written to it, 0.
TEST(the_frame_interrupt_stays_pending_until_acknowledged) {
    ql::QL repeating(rom_with_level_2_handler({
                         0x52B9, 0x0003, 0x0000,   // ADDQ.L #1,($00030000).L
                         0x4E73,                   // RTE
                     }),
                     128);
    repeating.reset();
    repeating.run_frames(1);
    CHECK(long_at(repeating, COUNTER) > 100u);

    ql::QL reading(rom_running({
                       0x13F9, 0x0001, 0x8021, 0x0003, 0x0004,   // MOVE.B ($00018021).L,($00030004).L
                       0x13FC, 0x0008, 0x0001, 0x8021,           // MOVE.B #$08,($00018021).L
                       0x13F9, 0x0001, 0x8021, 0x0003, 0x0005,   // MOVE.B ($00018021).L,($00030005).L
                       qltest::STOP, qltest::SUPERVISOR,
                   }),
                   128);
    reading.poke(0x030005, 0xFF);
    reading.reset();
    reading.run_frames(1);
    CHECK_EQ(reading.peek(0x030004), uint8_t{0x08});
    CHECK_EQ(reading.peek(0x030005), uint8_t{0x00});
}

// The real-time clock reads as four bytes from $18000, most significant
// first. Set to $12345678 at power-on, it reads that through the first frame;
// after 51 frames, 7,650,000 clocks, a second of emulated time has passed and
// it reads one more. It counts from 1 January 1961: a Unix time converts by
// adding 3,287 days, 283,996,800 seconds, as MAME does.
TEST(the_real_time_clock_counts_seconds_in_emulated_time) {
    ql::QL machine(rom_running({
                       0x23F9, 0x0001, 0x8000, 0x0003, 0x0000,   // MOVE.L ($00018000).L,($00030000).L
                       0x60F4,                                   // BRA.S $100: $10C - 12
                   }),
                   128);
    machine.set_rtc(0x12345678);
    machine.reset();
    machine.run_frames(1);
    CHECK_EQ(long_at(machine, COUNTER), 0x12345678u);
    machine.run_frames(50);
    CHECK_EQ(long_at(machine, COUNTER), 0x12345679u);

    CHECK_EQ(ql::rtc_from_unix(0), uint32_t{283'996'800});
    CHECK_EQ(ql::rtc_from_unix(1'700'000'000), uint32_t{1'983'996'800});
}

// With no microdrives, the microdrive gap is always present, as in MAME, whose
// ZX8302 starts with it set and has nothing to clear it: status bit 3 at
// $18020 reads 1. The gap interrupt, bit 0 of $18021, is raised as soon as its
// mask bit, 5, is set, and again whenever it is acknowledged while the mask
// stays set ("The gap interrupt regenerates as long as the gap level is
// present and the mask is enabled. Minerva relies on this.", MAME's
// irq_acknowledge_w). Acknowledged with the mask cleared, it goes.
TEST(with_no_microdrives_the_gap_is_always_present) {
    ql::QL machine(rom_running({0x60FE}), 128);   // BRA.S *
    machine.reset();
    auto read = [&machine](uint32_t address) {
        return machine.read(ql::BusCycle{address, ql::FunctionCode::SupervisorData, 0});
    };
    auto write = [&machine](uint32_t address, uint8_t value) {
        machine.write(ql::BusCycle{address, ql::FunctionCode::SupervisorData, 0}, value);
    };
    CHECK_EQ(read(0x18020) & 0x08, 0x08);
    CHECK_EQ(read(0x18021), uint8_t{0x00});
    write(0x18021, 0x20);
    CHECK_EQ(read(0x18021), uint8_t{0x01});
    write(0x18021, 0x21);
    CHECK_EQ(read(0x18021), uint8_t{0x01});
    write(0x18021, 0x01);
    CHECK_EQ(read(0x18021), uint8_t{0x00});
}

RUN_TESTS()
