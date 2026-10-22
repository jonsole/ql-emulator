// What SingleStepTests cannot show about program flow: DBcc with its counter
// running out. The suite's DBcc cases never start with the low word of Dn at
// zero, so the third of DBcc's outcomes is checked here, by hand.
//
// Expected values are worked out from the M68000 User's Manual: what DBcc is
// defined to do, and the 68000's time made into the 68008's by counting each
// word moved as two byte cycles of 4 clocks.

#include "m68008.h"
#include "test_main.h"
#include "test_memory.h"

using qltest::Memory;

namespace {

constexpr uint32_t PROGRAM = 0x000400;
constexpr uint16_t NOP = 0x4E71;

} // namespace

// DBF D0,*-2 (0101 0001 1100 1000, displacement $FFFC): the condition is
// never true, so D0's low word counts down; from 0 it runs out to $FFFF, the
// branch is not taken, and the next instruction is the NOP after the
// displacement word. The high word of D0 is untouched.
//
// Time: 26(6/0) on the 68008 (User's Manual, Table 7-10, DBcc, CC false,
// not taken) -- the 68000's 14(3/0), Table 8-9, with its three words read
// as six byte cycles, 12 more.
TEST(dbcc_falls_through_when_the_count_runs_out) {
    Memory memory;
    memory.set_word(PROGRAM, 0x51C8);
    memory.set_word(PROGRAM + 2, 0xFFFC);
    memory.set_word(PROGRAM + 4, NOP);
    memory.set_word(PROGRAM + 6, NOP);
    ql::M68008 cpu(memory);
    ql::Registers r{};
    r.sr = 0x2700;
    r.d[0] = 0x12340000;
    r.ir = 0x51C8;
    r.irc = 0xFFFC;
    r.pc = PROGRAM + 4;   // the queue full: see Registers::pc
    cpu.set_registers(r);

    cpu.step();

    r = cpu.registers();
    CHECK_EQ(r.d[0], 0x1234FFFFu);
    CHECK_EQ(r.pc, PROGRAM + 8);
    CHECK_EQ(r.ir, NOP);
    CHECK_EQ(r.irc, NOP);
    CHECK_EQ(cpu.clock(), uint64_t{26});
    CHECK_EQ(memory.reads, 6);
    CHECK_EQ(memory.writes, 0);
}

RUN_TESTS()
