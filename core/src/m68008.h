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
#include <stdexcept>

namespace ql {

/// Thrown by M68008::step for an opcode the 68000 defines but the core does
/// not run yet. Saying so beats running on in the wrong state; it goes when
/// Stage 1's instruction tickets are done.
class NotImplemented : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

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
    /// After an instruction run with T set, the step after it is the trace
    /// exception alone.
    void step();

    /// True after STOP, until an interrupt or reset starts the processor
    /// again.
    bool stopped() const { return stopped_; }

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

    /// Runs one instruction, by the opcode's decoding.
    void execute(uint16_t opcode);

    // The instructions, one routine each, as m68008_decode.h names them, in
    // a source file per group (m68008_internal.h lists them). Each is handed
    // an opcode the decoding has already found valid.
    static Size move_size(uint16_t opcode);
    void move(uint16_t opcode, Size size);
    void moveq(uint16_t opcode);
    void movea(uint16_t opcode);
    void lea(uint16_t opcode);
    void pea(uint16_t opcode);
    void trap(uint16_t opcode);
    void trapv();
    void chk(uint16_t opcode);
    void rte();
    void stop();
    void reset_instruction();   ///< RESET, not a reset of the CPU
    void move_usp(uint16_t opcode);
    void logic_to_status(uint16_t opcode, bool to_sr);
    void move_to_status(uint16_t opcode, bool to_sr);
    void move_from_sr(uint16_t opcode);
    void bcc(uint16_t opcode);
    void bsr(uint16_t opcode);
    void dbcc(uint16_t opcode);
    void jmp(uint16_t opcode);
    void jsr(uint16_t opcode);
    void rts();
    void rtr();
    void link(uint16_t opcode);
    void unlk(uint16_t opcode);
    void add_sub(uint16_t opcode);
    void addq_subq(uint16_t opcode);
    void addi_subi(uint16_t opcode);
    void adda_suba(uint16_t opcode);
    void addx_subx(uint16_t opcode);
    /// Reads -(An) as ADDX and SUBX do: An decremented, then the operand
    /// read, a long's low word first. `address` is set to where it was.
    uint32_t read_predecrement_low_first(unsigned reg, Size size, uint32_t& address);

    /// The size field most instructions keep in bits 7-6: 00 byte, 01 word,
    /// 10 long.
    static Size size_field(uint16_t opcode);
    /// dst + src, or dst - src, at the given size, setting N, Z, V and C --
    /// and X, unless `affects_x` is false, as for CMP. With `extend`, X is
    /// added or subtracted too and Z can only be cleared, as ADDX, SUBX and
    /// NEGX do, so that a multi-word result is zero only if every word is.
    uint32_t arithmetic(uint32_t dst, uint32_t src, Size size, bool subtract, bool extend, bool affects_x);
    /// The end of a read-modify-write: the prefetch, then the result written
    /// back. To memory a long goes low word first, unlike MOVE's
    /// (SingleStepTests). A long result in a register costs the internal
    /// clocks given, which differ from one instruction to the next.
    void finish_read_modify_write(const Operand& dest, Size size, uint32_t value, unsigned long_register_clocks);
    void neg_negx_clr(uint16_t opcode);
    void tst(uint16_t opcode);
    void cmp(uint16_t opcode);
    void cmpa(uint16_t opcode);
    void cmpi(uint16_t opcode);
    void cmpm(uint16_t opcode);
    void ext(uint16_t opcode);
    void swap(uint16_t opcode);
    void exg(uint16_t opcode);
    /// Pops a long from the active stack, high word first.
    uint32_t pop_long();
    /// Where a JMP or JSR goes, spending its internal clocks. Unlike
    /// control_address, it does not refill the queue as it takes extension
    /// words: the queue is about to be filled at the target. `next` is set
    /// to the address after the instruction, JSR's return address.
    uint32_t jump_target(unsigned mode, unsigned reg, uint32_t& next);
    /// Where a Bcc, BRA or BSR goes, from its byte or word displacement.
    uint32_t branch_target(uint16_t opcode) const;

    /// Whether condition cc (0-15, T F HI LS CC CS NE EQ VC VS PL MI GE LT GT
    /// LE) holds for the current condition codes.
    bool condition(unsigned cc) const;

    /// Group 1 and 2 exception processing: the supervisor state, the
    /// three-word frame on the supervisor stack, the vector fetch and a fresh
    /// prefetch queue at the handler. The caller spends whatever internal
    /// clocks come first.
    void exception(unsigned vector, uint32_t return_address);
    /// Fills both IR and IRC from the given address, leaving PC 4 past it.
    void refill_queue(uint32_t address);
    /// The exception for an instruction that cannot run -- illegal, line A
    /// or F, or privileged in the user state -- returning to its opcode.
    void instruction_exception(unsigned vector);
    /// For a privileged instruction: true in the supervisor state; otherwise
    /// takes the privilege-violation exception and returns false.
    bool supervisor_or_trap();

    Bus& bus_;
    Registers regs_{};
    uint64_t clock_ = 0;
    bool stopped_ = false;
    /// An instruction ran with T set; its trace exception is the next step.
    bool trace_pending_ = false;
};

} // namespace ql
