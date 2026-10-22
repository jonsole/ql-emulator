// Interrupts, which SingleStepTests does not cover.
//
// Each test runs a few words of program on a plain memory, with the IPL lines
// set through M68008::set_interrupt_level. Every expected value is worked out
// from the M68000 User's Manual (M68000UM/AD rev. 8), and each test names the
// section it rests on:
//
//   6.3.2  Interrupts: levels 1-7 against the mask in SR, level 7
//          non-maskable and taken on its rising edge; the new SR (S set, T
//          cleared, the mask raised to the level); the acknowledge cycle with
//          the level on A1-A3; autovectors 25-31 for levels 1-7.
//   6.2    Exception processing: the three-word group 1 and 2 frame.
//   Table 7-15  Exception processing times on the 68008: Interrupt 72(9/6),
//          the acknowledge cycle taken as 4 clocks.
//
// The 68000's 44(5/3) (Table 8-14) gives the same by counting each word moved
// as two byte cycles of 4 clocks: the acknowledge reads a byte either way;
// the vector (2 words), the prefetch (2) and the frame (3) are 7 words, 28
// more, with 9 reads and 6 writes on the 8-bit bus.

#include "m68008.h"
#include "test_main.h"
#include "test_memory.h"

using qltest::Memory;

namespace {

constexpr uint32_t PROGRAM = 0x000400;
constexpr uint32_t STACK_TOP = 0x008000;
constexpr uint16_t NOP = 0x4E71;

/// An interrupt's processing on the 68008: see the header comment.
constexpr uint64_t INTERRUPT_CLOCKS = 72;

/// A NOP: one word fetched, two byte cycles.
constexpr uint64_t NOP_CLOCKS = 8;

/// Autovector n's handler, at 0x1000 per level so each is told apart.
constexpr uint32_t handler_for(unsigned level) {
    return 0x001000 * (level + 1);
}

/// A machine with every autovector set, NOPs at each handler, and `first`
/// and `second` at PROGRAM, with the CPU in the supervisor state at `sr`
/// about to run `first`.
struct Machine {
    Memory memory;
    ql::M68008 cpu{memory};

    Machine(uint16_t first, uint16_t second, uint16_t sr) {
        for (unsigned level = 1; level <= 7; level++) {
            uint32_t vector = 24 + level;
            memory.set_word(vector * 4, static_cast<uint16_t>(handler_for(level) >> 16));
            memory.set_word(vector * 4 + 2, static_cast<uint16_t>(handler_for(level)));
            memory.set_word(handler_for(level), NOP);
            memory.set_word(handler_for(level) + 2, NOP);
        }
        memory.set_word(PROGRAM, first);
        memory.set_word(PROGRAM + 2, second);
        memory.set_word(PROGRAM + 4, NOP);
        memory.set_word(PROGRAM + 6, NOP);
        ql::Registers r{};
        r.sr = sr;
        r.ssp = STACK_TOP;
        r.ir = first;
        r.irc = second;
        r.pc = PROGRAM + 4;   // the queue full: see Registers::pc
        cpu.set_registers(r);
    }

    /// Checks the frame, the new SR and the queue at the handler after one
    /// interrupt from an empty supervisor stack.
    void check_interrupt(unsigned level, uint16_t stacked_sr, uint32_t stacked_pc) {
        ql::Registers r = cpu.registers();
        CHECK_EQ(r.ssp, STACK_TOP - 6);
        CHECK_EQ(memory.word(STACK_TOP - 6), stacked_sr);
        CHECK_EQ(memory.word(STACK_TOP - 4), static_cast<uint16_t>(stacked_pc >> 16));
        CHECK_EQ(memory.word(STACK_TOP - 2), static_cast<uint16_t>(stacked_pc));
        // S set, T cleared, the mask raised to the level; the condition codes
        // as they were.
        uint16_t expected_sr = static_cast<uint16_t>((stacked_sr & 0x00FF) | 0x2000 | level << 8);
        CHECK_EQ(r.sr, expected_sr);
        CHECK_EQ(r.pc, handler_for(level) + 4);
        CHECK_EQ(r.ir, NOP);
        CHECK_EQ(r.irc, NOP);
    }
};

} // namespace

// 6.3.2: a level above the mask is taken when the current instruction ends.
// Level 2 with the mask at 0: after the NOP, the acknowledge cycle with 2 on
// A1-A3 (the other address lines high, A0 high for the byte), autovector 26,
// and the NOP after it stacked as the return address. 6.2 for the frame,
// Table 7-15 for the time.
TEST(an_interrupt_above_the_mask_is_taken_after_the_instruction) {
    Machine m(NOP, NOP, 0x2004);   // mask 0, Z set to see it kept
    m.cpu.step();
    m.cpu.set_interrupt_level(2);
    m.cpu.step();

    m.check_interrupt(2, 0x2004, PROGRAM + 2);
    CHECK_EQ(m.memory.acknowledges.size(), size_t{1});
    if (m.memory.acknowledges.size() == 1) CHECK_EQ(m.memory.acknowledges[0].address, 0xFFFFF5u);
    CHECK_EQ(m.cpu.clock(), NOP_CLOCKS + INTERRUPT_CLOCKS);
}

// 6.3.2: a level at or below the mask is held off -- it stays on the lines,
// and is taken once the level rises above the mask. Mask 3: levels 3 and 2
// let the NOPs run; level 4 is taken after them.
TEST(an_interrupt_at_or_below_the_mask_is_held_off) {
    Machine m(NOP, NOP, 0x2300);
    m.cpu.set_interrupt_level(3);
    m.cpu.step();
    m.cpu.set_interrupt_level(2);
    m.cpu.step();
    CHECK_EQ(m.cpu.registers().pc, PROGRAM + 8);
    CHECK_EQ(m.memory.acknowledges.size(), size_t{0});
    CHECK_EQ(m.cpu.clock(), 2 * NOP_CLOCKS);

    m.cpu.set_interrupt_level(4);
    m.cpu.step();
    m.check_interrupt(4, 0x2300, PROGRAM + 4);
    CHECK_EQ(m.cpu.clock(), 2 * NOP_CLOCKS + INTERRUPT_CLOCKS);
}

// 6.3.2: level 7 cannot be masked, and is taken each time the lines rise to
// it -- once, not again while they are held there. With the mask already at
// 7 it is taken at once; the handler's NOP then runs with the lines still at
// 7; only after they drop and rise again is it taken a second time.
TEST(level_7_is_taken_whatever_the_mask_once_per_rise) {
    Machine m(NOP, NOP, 0x2700);
    m.cpu.set_interrupt_level(7);
    m.cpu.step();
    m.check_interrupt(7, 0x2700, PROGRAM);
    CHECK_EQ(m.memory.acknowledges.size(), size_t{1});
    if (m.memory.acknowledges.size() == 1) CHECK_EQ(m.memory.acknowledges[0].address, 0xFFFFFFu);

    m.cpu.step();
    CHECK_EQ(m.cpu.registers().pc, handler_for(7) + 6);
    CHECK_EQ(m.memory.acknowledges.size(), size_t{1});

    m.cpu.set_interrupt_level(0);
    m.cpu.set_interrupt_level(7);
    m.cpu.step();
    CHECK_EQ(m.memory.acknowledges.size(), size_t{2});
    CHECK_EQ(m.cpu.registers().pc, handler_for(7) + 4);
}

// STOP #$2300 (User's Manual 4, STOP) loads SR and waits; 6.3.2 for what
// wakes it. Level 2 is at or below the new mask of 3, so the processor stays
// stopped, 4 clocks a step; level 5 is taken, stacking the address after the
// STOP and its immediate, so that the RTE goes on past it.
TEST(stop_waits_for_an_interrupt_above_its_mask) {
    Machine m(0x4E72, 0x2300, 0x2700);
    m.cpu.step();
    CHECK(m.cpu.stopped());
    m.cpu.set_interrupt_level(2);
    m.cpu.step();
    CHECK(m.cpu.stopped());
    CHECK_EQ(m.memory.acknowledges.size(), size_t{0});
    CHECK_EQ(m.cpu.clock(), uint64_t{8});   // STOP's 4, and one stopped step

    m.cpu.set_interrupt_level(5);
    m.cpu.step();
    CHECK(!m.cpu.stopped());
    m.check_interrupt(5, 0x2300, PROGRAM + 4);
    CHECK_EQ(m.cpu.clock(), 8 + INTERRUPT_CLOCKS);
}

RUN_TESTS()
