#pragma once
// The Motorola 68008, clocked per bus access.
//
// The 68008 is a 68000 inside -- the same registers, the same microcode, the
// same two-word prefetch -- behind an 8-bit data bus. Every word the 68000
// would move in one bus cycle, the 68008 moves as two byte cycles, high byte
// first, and a long word as four. So the core is built around the byte cycle:
// each is an event of 4 clocks plus whatever wait states the Bus asks for, and
// that is the only place time passes apart from the internal cycles an
// instruction spends between accesses. Those are counted but not modelled at
// the pins (docs/plan.md, "The machine").
//
// The address the core puts on the bus is 24 bits, the 68000's A1-A23 with A0
// added back. The QL's 48-pin 68008 drives only A0-A19; dropping the top bits
// is the machine's job, not the CPU's, so the same core runs SingleStepTests,
// whose addresses span the 68000's whole 16 MB.

#include <cstdint>

namespace ql {

/// FC2-FC0: what kind of access a bus cycle is, as the 68000 family drives it.
enum class FunctionCode : uint8_t {
    UserData = 1,
    UserProgram = 2,
    SupervisorData = 5,
    SupervisorProgram = 6,
    InterruptAcknowledge = 7,
};

/// One byte cycle as the CPU drives it.
struct BusCycle {
    uint32_t address;       ///< 24 bits; see the header comment
    FunctionCode function_code;
    uint64_t clock;         ///< the CPU clock at which the cycle begins
};

/// Everything outside the CPU. A machine implements this to decode addresses
/// and to hold the CPU off the bus; a test implements it to record what the
/// CPU did.
class Bus {
public:
    virtual ~Bus() = default;

    virtual uint8_t read(const BusCycle& cycle) = 0;
    virtual void write(const BusCycle& cycle, uint8_t data) = 0;

    /// Clocks this cycle is held beyond the 68008's minimum of 4. Asked before
    /// the cycle is carried out, with the clock it would begin at: the QL's
    /// ZX8301 delays any access that falls in one of its display slots.
    virtual unsigned wait_states(const BusCycle& cycle) {
        (void)cycle;
        return 0;
    }
};

/// The programmer-visible state, plus the prefetch queue, laid out the way
/// SingleStepTests describes a 68000: A7 is not stored on its own but is
/// whichever of USP and SSP the S bit selects.
struct Registers {
    uint32_t d[8];
    uint32_t a[7];          ///< A0-A6
    uint32_t usp;
    uint32_t ssp;
    uint16_t sr;
    /// The address of the next prefetch, not of the instruction being run:
    /// with the queue full, that is the instruction's address plus 4.
    uint32_t pc;
    uint16_t ir;            ///< the opcode of the instruction about to run
    uint16_t irc;           ///< the word after it, already fetched
};

/// An operation's size, in bytes.
enum class Size : uint8_t {
    Byte = 1,
    Word = 2,
    Long = 4,
};

class M68008 {
public:
    explicit M68008(Bus& bus);

    Registers registers() const { return regs_; }
    void set_registers(const Registers& regs) { regs_ = regs; }

    /// Clocks run since construction, at 7.5 MHz on the QL.
    uint64_t clock() const { return clock_; }

    /// Runs the instruction in IR to completion, leaving the next one in IR.
    void step();

private:
    uint8_t read_byte(uint32_t address, FunctionCode function_code);
    uint16_t read_word(uint32_t address, FunctionCode function_code);
    void write_byte(uint32_t address, uint8_t data, FunctionCode function_code);
    void write_word(uint32_t address, uint16_t data, FunctionCode function_code);
    FunctionCode data_space() const;
    /// Pushes a long onto the active stack, high word first: PEA's order in
    /// SingleStepTests. Not every push uses it -- MOVE.L to -(An) writes the
    /// low word first.
    void push_long(uint32_t value);
    /// Refills the queue: IR takes IRC, and IRC the word at PC.
    void prefetch();
    /// Takes the extension word waiting in IRC and fetches the next in its
    /// place, which is how the 68000 reads every word after the opcode.
    uint16_t take_extension();
    /// Clocks spent inside the CPU with nothing on the bus.
    void idle(unsigned clocks) { clock_ += clocks; }
    FunctionCode program_space() const;

    /// A0-A7, with A7 the stack pointer the S bit selects.
    uint32_t& address_register(unsigned n);
    /// The address a control mode names -- (An), (d16,An), (d8,An,Xn),
    /// (xxx).W, (xxx).L, (d16,PC) and (d8,PC,Xn) -- taking its extension
    /// words and spending its internal clocks as the 68000 does.
    uint32_t control_address(unsigned mode, unsigned reg);
    /// The base plus an index extension word's register and 8-bit displacement.
    uint32_t indexed(uint32_t base, uint16_t extension);

    /// Where an effective address leads, once its extension words are taken.
    struct Operand {
        enum class Kind : uint8_t { DataRegister, AddressRegister, Memory, Immediate };
        Kind kind;
        unsigned reg;                ///< for the register kinds
        uint32_t address;            ///< for Memory
        FunctionCode space;          ///< for Memory: PC-relative operands are program space
        uint32_t immediate;          ///< for Immediate
    };
    /// Resolves an operand to be read (or read and then written): takes its
    /// extension words, spends its internal clocks, and applies (An)+ and
    /// -(An) to the register.
    Operand resolve(unsigned mode, unsigned reg, Size size);
    uint32_t read(const Operand& operand, Size size);
    void write(const Operand& operand, Size size, uint32_t value);
    uint32_t read_memory(uint32_t address, Size size, FunctionCode space);
    void write_memory(uint32_t address, Size size, uint32_t value);
    /// How far (An)+ and -(An) move An: the size, except that A7 moves a
    /// byte operation by 2 to keep the stack word-aligned.
    uint32_t step_size(unsigned reg, Size size) const;
    /// N and Z from a result of the given size, V and C cleared, X untouched:
    /// the condition codes of every move.
    void set_move_flags(uint32_t value, Size size);

    void move(uint16_t opcode, Size size);

    Bus& bus_;
    Registers regs_{};
    uint64_t clock_ = 0;
};

} // namespace ql
