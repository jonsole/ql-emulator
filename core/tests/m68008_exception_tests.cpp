// What SingleStepTests cannot show about exceptions: the ILLEGAL opcode and the
// opcodes the 68000 leaves unassigned (the suite has files only for lines A and
// F), trace (its tests end with the instruction, before a trace would be
// taken), and the stopped state after STOP (its tests end as the processor
// stops).
//
// Each test runs a few words of program on a plain memory. Expected values are
// worked out from the M68000 User's Manual: the vector table, the three-word
// group 1 and 2 frame, and the 68000's exception times, made into the 68008's
// by counting each word moved as two byte cycles of 4 clocks.

#include "m68008.h"
#include "test_main.h"
#include "test_memory.h"

using qltest::Memory;

namespace {

constexpr uint32_t PROGRAM = 0x000400;
constexpr uint32_t STACK_TOP = 0x008000;
constexpr uint32_t ILLEGAL_HANDLER = 0x001000;
constexpr uint32_t TRACE_HANDLER = 0x002000;
constexpr uint16_t NOP = 0x4E71;

/// A 68000's exception, group 1 or 2: 34 clocks, 4 words read and 3 written
/// (User's Manual, exception processing times, 34(4/3)). On the 68008 each of
/// those 7 words is two byte cycles, 4 clocks more apiece: 34 + 28.
constexpr uint64_t EXCEPTION_CLOCKS = 62;

/// A machine with the illegal-instruction (4) and trace (9) vectors set, NOPs
/// at both handlers, and `first` and `second` at PROGRAM, with the CPU in the
/// supervisor state about to run `first`.
struct Machine {
    Memory memory;
    ql::M68008 cpu{memory};

    Machine(uint16_t first, uint16_t second, uint16_t sr) {
        memory.set_word(4 * 4, 0);
        memory.set_word(4 * 4 + 2, static_cast<uint16_t>(ILLEGAL_HANDLER));
        memory.set_word(9 * 4, 0);
        memory.set_word(9 * 4 + 2, static_cast<uint16_t>(TRACE_HANDLER));
        for (uint32_t handler : {ILLEGAL_HANDLER, TRACE_HANDLER}) {
            memory.set_word(handler, NOP);
            memory.set_word(handler + 2, NOP);
        }
        memory.set_word(PROGRAM, first);
        memory.set_word(PROGRAM + 2, second);
        memory.set_word(PROGRAM + 4, NOP);
        ql::Registers r{};
        r.sr = sr;
        r.ssp = STACK_TOP;
        r.ir = first;
        r.irc = second;
        r.pc = PROGRAM + 4;   // the queue full: see Registers::pc
        cpu.set_registers(r);
    }

    /// Checks the frame on top of the supervisor stack and the queue at the
    /// handler, after one exception from an empty stack.
    void check_exception(uint16_t stacked_sr, uint32_t stacked_pc, uint32_t handler) {
        ql::Registers r = cpu.registers();
        CHECK_EQ(r.ssp, STACK_TOP - 6);
        CHECK_EQ(memory.word(STACK_TOP - 6), stacked_sr);
        CHECK_EQ(memory.word(STACK_TOP - 4), static_cast<uint16_t>(stacked_pc >> 16));
        CHECK_EQ(memory.word(STACK_TOP - 2), static_cast<uint16_t>(stacked_pc));
        CHECK_EQ(r.pc, handler + 4);
        CHECK_EQ(r.ir, NOP);
        CHECK_EQ(r.irc, NOP);
    }
};

constexpr uint16_t SUPERVISOR = 0x2700;

} // namespace

// ILLEGAL (0100 1010 1111 1100) is the one opcode Motorola promises will never
// be anything else. It takes vector 4, stacking its own address.
TEST(illegal_takes_vector_4) {
    Machine m(0x4AFC, NOP, SUPERVISOR);
    m.cpu.step();
    m.check_exception(SUPERVISOR, PROGRAM, ILLEGAL_HANDLER);
    CHECK_EQ(m.cpu.registers().sr, SUPERVISOR);
    CHECK_EQ(m.cpu.clock(), EXCEPTION_CLOCKS);
}

// Opcodes the 68000 does not define, outside lines A and F, take the same
// exception. Each is unassigned by the 68000's encoding tables in the User's
// Manual; most are instructions on later processors, which is what makes them
// worth checking.
TEST(unassigned_opcodes_take_vector_4) {
    struct Unassigned {
        uint16_t opcode;
        const char* why;
    };
    const Unassigned opcodes[] = {
        {0x4E7A, "MOVEC (68010): 0100 1110 0111 1010 is not a 68000 instruction"},
        {0x4E74, "RTD (68010): likewise"},
        {0x4848, "BKPT (68010): to a 68000, PEA with An, not a control mode"},
        {0x49C0, "EXTB.L (68020): to a 68000, LEA with Dn, not a control mode"},
        {0x06C0, "RTM (68020): to a 68000, ADDI with size 11, which is no size"},
        {0xE8C0, "BFTST (68020): to a 68000, a memory shift with Dn, and type 100"},
        {0x1008, "MOVE.B A0,D0: no byte operation on An"},
        {0x4AFD, "TAS #imm: TAS's encoding with a mode it cannot write"},
    };
    for (const Unassigned& u : opcodes) {
        Machine m(u.opcode, NOP, SUPERVISOR);
        try {
            m.cpu.step();
        } catch (const std::exception& e) {
            CHECK(!"threw instead of taking the exception");
            std::printf("      %04x, %s: %s\n", u.opcode, u.why, e.what());
            continue;
        }
        ql::Registers r = m.cpu.registers();
        if (r.pc != ILLEGAL_HANDLER + 4) std::printf("      %04x, %s: not taken\n", u.opcode, u.why);
        m.check_exception(SUPERVISOR, PROGRAM, ILLEGAL_HANDLER);
        CHECK_EQ(m.cpu.clock(), EXCEPTION_CLOCKS);
    }
}

// With T set, each instruction is followed by a trace exception, vector 9,
// returning to the next instruction with T cleared in the handler. The core
// takes it as the next step(), on its own: SingleStepTests' cases end with the
// instruction, before the trace would go out.
TEST(an_instruction_run_with_t_set_is_followed_by_a_trace) {
    constexpr uint16_t TRACING = 0xA700;   // T and S, interrupts masked
    Machine m(NOP, NOP, TRACING);
    m.cpu.step();
    // The NOP itself: one word fetched, two byte cycles.
    CHECK_EQ(m.cpu.registers().pc, PROGRAM + 6);
    CHECK_EQ(m.cpu.clock(), uint64_t{8});

    m.cpu.step();
    m.check_exception(TRACING, PROGRAM + 2, TRACE_HANDLER);
    CHECK_EQ(m.cpu.registers().sr, SUPERVISOR);
    CHECK_EQ(m.cpu.clock(), 8 + EXCEPTION_CLOCKS);

    // The handler runs untraced.
    m.cpu.step();
    CHECK_EQ(m.cpu.registers().pc, TRACE_HANDLER + 6);
    CHECK_EQ(m.cpu.clock(), 8 + EXCEPTION_CLOCKS + 8);
}

// An illegal instruction is not traced: it never ran (M68000 Programmer's
// Reference Manual, 6.3.8, Tracing). After its exception the handler's first
// instruction runs.
TEST(an_illegal_instruction_is_not_traced) {
    Machine m(0x4AFC, NOP, 0xA700);
    m.cpu.step();
    m.check_exception(0xA700, PROGRAM, ILLEGAL_HANDLER);
    m.cpu.step();
    CHECK_EQ(m.cpu.registers().pc, ILLEGAL_HANDLER + 6);
    CHECK_EQ(m.cpu.clock(), EXCEPTION_CLOCKS + 8);
}

// STOP #imm loads SR and stops; only an interrupt or a reset starts the
// processor again (ticket #11). Until then each step spends 4 clocks with
// nothing on the bus, so a machine that only calls step() still sees time
// pass. The immediate here drops to the user state, where running STOP again
// would be a privilege violation: a stopped processor runs nothing.
TEST(stop_stays_stopped) {
    Machine m(0x4E72, 0x0000, SUPERVISOR);
    m.cpu.step();
    CHECK(m.cpu.stopped());
    CHECK_EQ(m.cpu.registers().sr, uint16_t{0x0000});
    CHECK_EQ(m.cpu.clock(), uint64_t{4});   // STOP: 4(0/0), nothing to fetch

    m.cpu.step();
    m.cpu.step();
    CHECK(m.cpu.stopped());
    CHECK_EQ(m.memory.reads + m.memory.writes, 0);
    CHECK_EQ(m.cpu.registers().pc, PROGRAM + 4);
    CHECK_EQ(m.cpu.registers().sr, uint16_t{0x0000});
    CHECK_EQ(m.cpu.clock(), uint64_t{12});
}

// Dividing by zero takes vector 5, returning to the next instruction, with
// the destination left alone. SingleStepTests has no case of it. Time: 38(4/3)
// on the 68000 (User's Manual, exception processing times), with nothing more
// for a register operand; 7 words on an 8-bit bus add 28: 66.
TEST(divide_by_zero_takes_vector_5) {
    constexpr uint32_t DIVIDE_HANDLER = 0x003000;
    const uint16_t opcodes[] = {
        0x80C1,   // DIVU D1,D0: 1000 000 011 000 001
        0x81C1,   // DIVS D1,D0: 1000 000 111 000 001
    };
    for (uint16_t opcode : opcodes) {
        Machine m(opcode, NOP, SUPERVISOR);
        m.memory.set_word(5 * 4, 0);
        m.memory.set_word(5 * 4 + 2, static_cast<uint16_t>(DIVIDE_HANDLER));
        m.memory.set_word(DIVIDE_HANDLER, NOP);
        m.memory.set_word(DIVIDE_HANDLER + 2, NOP);
        ql::Registers r = m.cpu.registers();
        r.d[0] = 0x12345678;
        r.d[1] = 0xABCD0000;   // the divisor is the low word: zero
        m.cpu.set_registers(r);

        m.cpu.step();

        m.check_exception(SUPERVISOR, PROGRAM + 2, DIVIDE_HANDLER);
        CHECK_EQ(m.cpu.registers().d[0], 0x12345678u);
        CHECK_EQ(m.cpu.clock(), uint64_t{66});
    }
}

RUN_TESTS()
