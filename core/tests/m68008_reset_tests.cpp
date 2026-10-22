// The reset sequence: what the 68008 does when RESET and HALT are released,
// as opposed to the RESET instruction. Expected values come from the M68000
// User's Manual (M68000UM/AD rev. 8): 6.3.1, Reset, for the state it leaves,
// and Table 7-15, Exception Processing Execution Times, for the 68008's time --
// the row marked **, "the time from when RESET and HALT are first sampled as
// negated to when instruction execution starts": 64(12/0). The 12 byte reads
// are the initial SSP (4), the initial PC (4) and the first two words of the
// program (4).

#include "m68008.h"
#include "test_main.h"
#include "test_memory.h"

using qltest::Memory;

namespace {

constexpr uint32_t INITIAL_SSP = 0x00028000;
constexpr uint32_t INITIAL_PC = 0x00000400;
constexpr uint16_t NOP = 0x4E71;
constexpr uint16_t MOVEQ_1_D0 = 0x7001;

/// A ROM's first eight bytes -- the reset vector -- and two words of program
/// at the initial PC.
void set_up_reset_vector(Memory& memory) {
    memory.set_word(0, static_cast<uint16_t>(INITIAL_SSP >> 16));
    memory.set_word(2, static_cast<uint16_t>(INITIAL_SSP));
    memory.set_word(4, static_cast<uint16_t>(INITIAL_PC >> 16));
    memory.set_word(6, static_cast<uint16_t>(INITIAL_PC));
    memory.set_word(INITIAL_PC, MOVEQ_1_D0);
    memory.set_word(INITIAL_PC + 2, NOP);
}

} // namespace

// 6.3.1: the supervisor state, trace off, the interrupt mask at 7; the SSP
// from the vector's first two words, the PC from its last two, read in the
// supervisor program space; execution starts at the PC. Table 7-15: 64(12/0).
TEST(reset_loads_the_stack_pointer_and_pc_from_the_vector) {
    Memory memory;
    set_up_reset_vector(memory);
    ql::M68008 cpu(memory);
    ql::Registers before{};
    before.sr = 0x8000;   // T set and the user state: reset must undo both
    cpu.set_registers(before);

    cpu.reset();

    ql::Registers r = cpu.registers();
    CHECK_EQ(r.ssp, INITIAL_SSP);
    CHECK_EQ(r.sr, uint16_t{0x2700});
    CHECK_EQ(r.ir, MOVEQ_1_D0);
    CHECK_EQ(r.irc, NOP);
    CHECK_EQ(r.pc, INITIAL_PC + 4);   // the queue full: see Registers::pc
    CHECK_EQ(cpu.clock(), uint64_t{64});
    CHECK_EQ(memory.reads, 12);
    CHECK_EQ(memory.writes, 0);
    for (ql::FunctionCode space : memory.read_spaces) {
        CHECK(space == ql::FunctionCode::SupervisorProgram);
    }

    // And execution starts at the PC: the MOVEQ runs.
    cpu.step();
    CHECK_EQ(cpu.registers().d[0], 1u);
}

// 6.3.1: "any processing in progress at the time of the reset is aborted".
// A processor stopped by STOP runs again, and a trace due from the
// instruction before the reset is not taken: the first step after reset runs
// the program's first instruction.
TEST(reset_ends_the_stopped_state_and_a_pending_trace) {
    constexpr uint32_t ELSEWHERE = 0x001000;
    constexpr uint16_t STOP = 0x4E72;
    {
        Memory memory;
        set_up_reset_vector(memory);
        ql::M68008 cpu(memory);
        ql::Registers r{};
        r.sr = 0x2700;
        r.ir = STOP;
        r.irc = 0x2700;
        r.pc = ELSEWHERE + 4;
        cpu.set_registers(r);
        cpu.step();
        CHECK(cpu.stopped());

        cpu.reset();
        CHECK(!cpu.stopped());
        cpu.step();
        CHECK_EQ(cpu.registers().d[0], 1u);
    }
    {
        Memory memory;
        set_up_reset_vector(memory);
        memory.set_word(ELSEWHERE + 4, NOP);
        ql::M68008 cpu(memory);
        ql::Registers r{};
        r.sr = 0xA700;   // T set: the NOP leaves a trace due
        r.ir = NOP;
        r.irc = NOP;
        r.pc = ELSEWHERE + 4;
        cpu.set_registers(r);
        cpu.step();

        cpu.reset();
        cpu.step();
        CHECK_EQ(cpu.registers().d[0], 1u);
        CHECK_EQ(cpu.registers().pc, INITIAL_PC + 6);
    }
}

// Level 7 is taken once per rise to it (6.3.2). A reset abandons everything
// in progress (6.3.1), the record that level 7 has been taken included, so
// with the lines still held at 7 it is taken again once the reset is done:
// the acknowledge comes from the first step after it, not from the program.
TEST(reset_forgets_that_level_7_was_taken) {
    constexpr uint32_t LEVEL_7_HANDLER = 0x003000;
    Memory memory;
    set_up_reset_vector(memory);
    memory.set_word(31 * 4, 0);
    memory.set_word(31 * 4 + 2, static_cast<uint16_t>(LEVEL_7_HANDLER));
    memory.set_word(LEVEL_7_HANDLER, NOP);
    memory.set_word(LEVEL_7_HANDLER + 2, NOP);
    ql::M68008 cpu(memory);
    cpu.reset();
    cpu.set_interrupt_level(7);
    cpu.step();
    CHECK_EQ(memory.acknowledges.size(), size_t{1});

    cpu.reset();
    cpu.step();
    CHECK_EQ(memory.acknowledges.size(), size_t{2});
    CHECK_EQ(cpu.registers().pc, LEVEL_7_HANDLER + 4);
}

RUN_TESTS()
