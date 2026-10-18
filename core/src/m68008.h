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
    /// Refills the queue: IR takes IRC, and IRC the word at PC.
    void prefetch();
    FunctionCode program_space() const;

    Bus& bus_;
    Registers regs_{};
    uint64_t clock_ = 0;
};

} // namespace ql
