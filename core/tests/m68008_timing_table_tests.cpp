// An independent check on the 68008 timing derivation. Everywhere else in
// Stage 1 a 68008 cycle count comes from SingleStepTests' 68000 counts, with
// each word access split into two byte cycles (m68008_timing.h). Here a
// handful of instructions are checked against Motorola's own figures for the
// 68008: Section 7, "8-Bit Instruction Execution Times", of the M68000 Family
// User's Manual (M68000UM/AD rev. 8), which gives each time as n(r/w) -- n
// clocks, r byte reads and w byte writes, with no wait states. Each test
// names the table and works its figure out for the operands it uses.
//
// Every time here agrees with the derivation. The one disagreement found in
// Section 7 is the address error: Table 7-15 gives 94(8/14), where the core
// spends 102 clocks from the start of the faulting access to the handler's
// first word. The reads and writes agree; the 8 clocks are the same 8 by
// which the 68000's own table (50(4/7), Table 8-14) falls short of the 58
// SingleStepTests records for the same sequence, so they are where the manual
// starts counting, not something an 8-bit bus changes. The core follows
// SingleStepTests, which records the real 68000's cycles.

#include "m68008.h"
#include "test_main.h"
#include "test_memory.h"

using qltest::Memory;

namespace {

constexpr uint32_t PROGRAM = 0x000400;
constexpr uint32_t STACK_TOP = 0x008000;
constexpr uint16_t NOP = 0x4E71;

/// One instruction at PROGRAM, queue full, in the supervisor state; NOPs
/// after it and at every address the tests jump to.
struct Machine {
    Memory memory;
    ql::M68008 cpu{memory};
    ql::Registers r{};

    Machine(uint16_t opcode, uint16_t extension = NOP) {
        memory.set_word(PROGRAM, opcode);
        memory.set_word(PROGRAM + 2, extension);
        for (uint32_t a = PROGRAM + 4; a < PROGRAM + 0x40; a += 2) memory.set_word(a, NOP);
        for (uint32_t a = 0x002000; a < 0x002010; a += 2) memory.set_word(a, NOP);
        r.sr = 0x2700;
        r.ssp = STACK_TOP;
        r.ir = opcode;
        r.irc = extension;
        r.pc = PROGRAM + 4;   // the queue full: see Registers::pc
    }

    /// Runs the instruction and checks it took `clocks`, with `reads` and
    /// `writes` byte cycles.
    void check(uint64_t clocks, int reads, int writes) {
        cpu.set_registers(r);
        cpu.step();
        CHECK_EQ(cpu.clock(), clocks);
        CHECK_EQ(memory.reads, reads);
        CHECK_EQ(memory.writes, writes);
    }
};

} // namespace

// A register-only instruction. ADD.L D1,D0 (1101 000 0 10 000 001): Table
// 7-5, ADD long op <ea>,Dn, 10(2/0) -- its footnote raising the base to 12
// for a register-direct source -- plus Dn's 0(0/0) from Table 7-1: 12(2/0).
TEST(add_long_register_to_register) {
    Machine m(0xD081);
    m.check(12, 2, 0);
}

// A read-modify-write in memory. ADD.W D1,(A0) (1101 001 1 01 010 000): Table
// 7-5, ADD word op Dn,<M>, 16(2/2), plus (An) word's 8(2/0) from Table 7-1:
// 24(4/2).
TEST(add_word_register_to_memory) {
    Machine m(0xD350);
    m.r.a[0] = 0x002000;
    m.check(24, 4, 2);
}

// A long, memory to memory. MOVE.L (A0),(A1) (00 10 001 010 010 000): Table
// 7-4, (An) to (An), 40(6/4) -- the long read, the long written, and the next
// opcode, all as bytes.
TEST(move_long_memory_to_memory) {
    Machine m(0x2290);
    m.r.a[0] = 0x002000;
    m.r.a[1] = 0x003000;
    m.check(40, 6, 4);
}

// Taken branches. Table 7-10: Bcc and BRA with a byte displacement, branch
// taken, 18(4/0). BRA.B *+4 (0110 0000 0000 0010), and BEQ.B *+4 (0110 0111
// ...) with Z set.
TEST(branches_taken) {
    Machine bra(0x6002);
    bra.check(18, 4, 0);
    Machine beq(0x6702);
    beq.r.sr = 0x2704;
    beq.check(18, 4, 0);
}

// Branches not taken. Table 7-10, branch not taken: with a byte
// displacement 12(2/0), with a word one 20(4/0) -- the displacement word is
// skipped by fetching past it. BEQ with Z clear.
TEST(branches_not_taken) {
    Machine byte(0x6702);
    byte.check(12, 2, 0);
    Machine word(0x6700, 0x0010);
    word.check(20, 4, 0);
}

// JSR (A0) (0100 1110 10 010 000): Table 7-11, JSR (An), 32(4/4) -- the
// return address pushed as four bytes, and the queue filled at the target.
TEST(jsr_address_register_indirect) {
    Machine m(0x4E90);
    m.r.a[0] = 0x002000;
    m.check(32, 4, 4);
}

// RTS (0100 1110 0111 0101): Table 7-13, 32(8/0) -- the return address read
// as four bytes and the queue filled there.
TEST(rts) {
    Machine m(0x4E75);
    m.memory.set_word(STACK_TOP, 0x0000);
    m.memory.set_word(STACK_TOP + 2, 0x2000);
    m.check(32, 8, 0);
}

// MOVEM.L D0-D1,(A0) (0100 1000 11 010 000, mask $0003): Table 7-11, MOVEM
// registers to memory, long, (An), 16+16n(4/4n). Two registers: 48(4/8).
TEST(movem_long_two_registers_to_memory) {
    Machine m(0x48D0, 0x0003);
    m.r.a[0] = 0x002000;
    m.check(48, 4, 8);
}

// MULU D1,D0 (1100 000 011 000 001): Table 7-5, MULU, 74(2/0) at most, and
// its footnote: 42+2n clocks, n the number of ones in the source. $FFFF:
// n = 16, 74; $5555: n = 8, 58. Dn adds 0(0/0) (Table 7-1).
TEST(mulu_time_follows_the_ones_in_the_source) {
    Machine worst(0xC0C1);
    worst.r.d[1] = 0xFFFF;
    worst.check(74, 2, 0);
    Machine half(0xC0C1);
    half.r.d[1] = 0x5555;
    half.check(58, 2, 0);
}

RUN_TESTS()
