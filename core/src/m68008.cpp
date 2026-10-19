#include "m68008.h"
#include "m68008_decode.h"

#include <stdexcept>

namespace ql {

namespace {

/// The shortest 68008 bus cycle, states S0-S7 with no wait states: the
/// 68008's read and write cycle timing in the M68000 User's Manual.
constexpr unsigned BYTE_CYCLE_CLOCKS = 4;

/// What a step costs while stopped. Not the chip's: a stopped 68000 simply
/// waits. One byte cycle's worth keeps the machine's time moving in steps no
/// coarser than an instruction's.
constexpr unsigned STOPPED_STEP_CLOCKS = 4;

/// The 68000 family drives A1-A23; with A0 that is 24 bits. See m68008.h.
constexpr uint32_t ADDRESS_MASK = 0x00FFFFFF;

// SR's bits: the system byte's trace and supervisor bits, and the condition
// codes.
constexpr uint16_t SR_T = 0x8000;
constexpr uint16_t SR_S = 0x2000;
constexpr uint16_t SR_X = 0x0010;
constexpr uint16_t SR_N = 0x0008;
constexpr uint16_t SR_Z = 0x0004;
constexpr uint16_t SR_V = 0x0002;
constexpr uint16_t SR_C = 0x0001;
constexpr uint16_t SR_CCR = 0x001F;
/// The bits of SR that exist on a 68000: T, S, the interrupt mask and the
/// condition codes. The rest read as zero whatever is written.
constexpr uint16_t SR_IMPLEMENTED = 0xA71F;

/// Exception vector numbers, from the M68000 User's Manual's vector table. The
/// vector's address is four times its number.
constexpr unsigned VECTOR_ILLEGAL = 4;
constexpr unsigned VECTOR_CHK = 6;
constexpr unsigned VECTOR_TRAPV = 7;
constexpr unsigned VECTOR_PRIVILEGE = 8;
constexpr unsigned VECTOR_TRACE = 9;
constexpr unsigned VECTOR_LINE_A = 10;
constexpr unsigned VECTOR_LINE_F = 11;
constexpr unsigned VECTOR_TRAP_0 = 32;

} // namespace

M68008::M68008(Bus& bus) : bus_(bus) {}

uint8_t M68008::read_byte(uint32_t address, FunctionCode function_code) {
    BusCycle cycle{address & ADDRESS_MASK, function_code, clock_};
    unsigned wait = bus_.wait_states(cycle);
    uint8_t data = bus_.read(cycle);
    clock_ += BYTE_CYCLE_CLOCKS + wait;
    return data;
}

uint16_t M68008::read_word(uint32_t address, FunctionCode function_code) {
    // Big-endian, and on an 8-bit bus the even (high) byte goes first.
    uint8_t high = read_byte(address, function_code);
    uint8_t low = read_byte(address + 1, function_code);
    return static_cast<uint16_t>(high << 8 | low);
}

void M68008::write_byte(uint32_t address, uint8_t data, FunctionCode function_code) {
    BusCycle cycle{address & ADDRESS_MASK, function_code, clock_};
    unsigned wait = bus_.wait_states(cycle);
    bus_.write(cycle, data);
    clock_ += BYTE_CYCLE_CLOCKS + wait;
}

void M68008::write_word(uint32_t address, uint16_t data, FunctionCode function_code) {
    write_byte(address, static_cast<uint8_t>(data >> 8), function_code);
    write_byte(address + 1, static_cast<uint8_t>(data), function_code);
}

void M68008::push_long(uint32_t value) {
    uint32_t& sp = address_register(7);
    sp -= 4;
    write_word(sp, static_cast<uint16_t>(value >> 16), data_space());
    write_word(sp + 2, static_cast<uint16_t>(value), data_space());
}

FunctionCode M68008::data_space() const {
    return (regs_.sr & SR_S) ? FunctionCode::SupervisorData : FunctionCode::UserData;
}

FunctionCode M68008::program_space() const {
    return (regs_.sr & SR_S) ? FunctionCode::SupervisorProgram : FunctionCode::UserProgram;
}

void M68008::prefetch() {
    regs_.ir = regs_.irc;
    regs_.irc = read_word(regs_.pc, program_space());
    regs_.pc += 2;
}

uint16_t M68008::take_extension() {
    uint16_t extension = regs_.irc;
    regs_.irc = read_word(regs_.pc, program_space());
    regs_.pc += 2;
    return extension;
}

uint32_t& M68008::address_register(unsigned n) {
    if (n < 7) return regs_.a[n];
    return (regs_.sr & SR_S) ? regs_.ssp : regs_.usp;
}

uint32_t M68008::indexed(uint32_t base, uint16_t extension) {
    // D/A in bit 15, the register in 14-12, W/L in 11, the displacement in 7-0.
    unsigned n = (extension >> 12) & 7;
    uint32_t index = (extension & 0x8000) ? address_register(n) : regs_.d[n];
    if (!(extension & 0x0800)) {
        index = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(index)));
    }
    uint32_t displacement = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(extension & 0xFF)));
    return base + index + displacement;
}

uint32_t M68008::control_address(unsigned mode, unsigned reg) {
    // The PC-relative modes are relative to the extension word itself, which
    // sits in IRC, one word behind the next prefetch address.
    switch (mode) {
    case 2:
        return address_register(reg);
    case 5: {
        uint32_t base = address_register(reg);
        return base + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(take_extension())));
    }
    case 6: {
        // The adder spends 2 clocks on the index before the extension word is
        // replaced (SingleStepTests: n2 before the refill).
        uint32_t base = address_register(reg);
        idle(2);
        return indexed(base, take_extension());
    }
    case 7:
        switch (reg) {
        case 0:
            return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(take_extension())));
        case 1: {
            uint32_t high = take_extension();
            return high << 16 | take_extension();
        }
        case 2: {
            uint32_t base = regs_.pc - 2;
            return base + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(take_extension())));
        }
        case 3: {
            uint32_t base = regs_.pc - 2;
            idle(2);
            return indexed(base, take_extension());
        }
        }
        break;
    }
    throw std::logic_error("M68008::control_address: not a control mode");
}

uint32_t M68008::step_size(unsigned reg, Size size) const {
    if (size == Size::Byte && reg == 7) return 2;
    return static_cast<uint32_t>(size);
}

M68008::Operand M68008::resolve(unsigned mode, unsigned reg, Size size) {
    Operand operand{};
    operand.reg = reg;
    operand.space = data_space();
    switch (mode) {
    case 0:
        operand.kind = Operand::Kind::DataRegister;
        return operand;
    case 1:
        operand.kind = Operand::Kind::AddressRegister;
        return operand;
    case 3: {
        uint32_t& an = address_register(reg);
        operand.kind = Operand::Kind::Memory;
        operand.address = an;
        an += step_size(reg, size);
        return operand;
    }
    case 4: {
        // The decrement costs 2 clocks before the operand is read.
        idle(2);
        uint32_t& an = address_register(reg);
        an -= step_size(reg, size);
        operand.kind = Operand::Kind::Memory;
        operand.address = an;
        return operand;
    }
    case 7:
        if (reg == 4) {
            // #imm: a byte is the low half of one extension word, a long two.
            operand.kind = Operand::Kind::Immediate;
            if (size == Size::Long) {
                uint32_t high = take_extension();
                operand.immediate = high << 16 | take_extension();
            } else {
                uint16_t word = take_extension();
                operand.immediate = size == Size::Byte ? (word & 0xFFu) : word;
            }
            return operand;
        }
        // The 68000 reads PC-relative operands from program space.
        if (reg == 2 || reg == 3) operand.space = program_space();
        break;
    }
    operand.kind = Operand::Kind::Memory;
    operand.address = control_address(mode, reg);
    return operand;
}

namespace {

uint32_t size_mask(Size size) {
    switch (size) {
    case Size::Byte: return 0xFF;
    case Size::Word: return 0xFFFF;
    case Size::Long: return 0xFFFFFFFF;
    }
    return 0;
}

} // namespace

uint32_t M68008::read(const Operand& operand, Size size) {
    switch (operand.kind) {
    case Operand::Kind::DataRegister: return regs_.d[operand.reg] & size_mask(size);
    case Operand::Kind::AddressRegister: return address_register(operand.reg) & size_mask(size);
    case Operand::Kind::Memory: return read_memory(operand.address, size, operand.space);
    case Operand::Kind::Immediate: return operand.immediate;
    }
    return 0;
}

void M68008::write(const Operand& operand, Size size, uint32_t value) {
    switch (operand.kind) {
    case Operand::Kind::DataRegister: {
        // Only the operation's size of Dn changes.
        uint32_t mask = size_mask(size);
        regs_.d[operand.reg] = (regs_.d[operand.reg] & ~mask) | (value & mask);
        return;
    }
    case Operand::Kind::AddressRegister:
        address_register(operand.reg) = value;
        return;
    case Operand::Kind::Memory:
        write_memory(operand.address, size, value);
        return;
    case Operand::Kind::Immediate:
        break;
    }
    throw std::logic_error("M68008::write: not a writable operand");
}

uint32_t M68008::read_memory(uint32_t address, Size size, FunctionCode space) {
    switch (size) {
    case Size::Byte: return read_byte(address, space);
    case Size::Word: return read_word(address, space);
    case Size::Long: {
        uint32_t high = read_word(address, space);
        return high << 16 | read_word(address + 2, space);
    }
    }
    return 0;
}

void M68008::write_memory(uint32_t address, Size size, uint32_t value) {
    switch (size) {
    case Size::Byte:
        write_byte(address, static_cast<uint8_t>(value), data_space());
        return;
    case Size::Word:
        write_word(address, static_cast<uint16_t>(value), data_space());
        return;
    case Size::Long:
        write_word(address, static_cast<uint16_t>(value >> 16), data_space());
        write_word(address + 2, static_cast<uint16_t>(value), data_space());
        return;
    }
}

void M68008::set_move_flags(uint32_t value, Size size) {
    uint32_t sign = (size_mask(size) >> 1) + 1;
    uint16_t ccr = regs_.sr & SR_X;
    if (value & sign) ccr |= SR_N;
    if ((value & size_mask(size)) == 0) ccr |= SR_Z;
    regs_.sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | ccr);
}

void M68008::move(uint16_t opcode, Size size) {
    unsigned source_mode = (opcode >> 3) & 7;
    unsigned dest_mode = (opcode >> 6) & 7;
    unsigned dest_reg = (opcode >> 9) & 7;
    Operand source = resolve(source_mode, opcode & 7, size);
    uint32_t value = read(source, size);
    set_move_flags(value, size);

    // Where the destination's write falls against the final prefetch depends
    // on its mode: the order below is SingleStepTests', which is the 68000's
    // microcode.
    switch (dest_mode) {
    case 0:
        write({Operand::Kind::DataRegister, dest_reg, 0, data_space(), 0}, size, value);
        prefetch();
        return;
    case 2:
    case 3: {
        Operand dest = resolve(dest_mode, dest_reg, size);
        write(dest, size, value);
        prefetch();
        return;
    }
    case 4: {
        // -(An) as a destination: no internal clocks, and the prefetch comes
        // before the write.
        uint32_t& an = address_register(dest_reg);
        an -= step_size(dest_reg, size);
        uint32_t address = an;
        prefetch();
        if (size == Size::Long) {
            // A long goes out low word first, from the top of the space down.
            write_word(address + 2, static_cast<uint16_t>(value), data_space());
            write_word(address, static_cast<uint16_t>(value >> 16), data_space());
        } else {
            write_memory(address, size, value);
        }
        return;
    }
    case 7:
        if (dest_reg == 1 && source_mode >= 2 && !(source_mode == 7 && (opcode & 7) == 4)) {
            // (xxx).L after a memory source: the write goes out with the low
            // word of the address still waiting in IRC, and only then is it
            // taken. After a register or an immediate, both words are taken
            // first.
            uint32_t high = take_extension();
            uint32_t address = high << 16 | regs_.irc;
            write_memory(address, size, value);
            take_extension();
            prefetch();
            return;
        }
        break;
    }
    write_memory(control_address(dest_mode, dest_reg), size, value);
    prefetch();
}

void M68008::exception(unsigned vector, uint32_t return_address) {
    // Exception processing is what ends STOP: a trace taken after STOP, or
    // an interrupt.
    stopped_ = false;
    uint16_t old_sr = regs_.sr;
    regs_.sr = static_cast<uint16_t>((regs_.sr | SR_S) & ~SR_T);

    // The frame is written PC low word first, then SR below it, then the PC's
    // high word between the two (SingleStepTests' bus order).
    regs_.ssp -= 6;
    write_word(regs_.ssp + 4, static_cast<uint16_t>(return_address), FunctionCode::SupervisorData);
    write_word(regs_.ssp, old_sr, FunctionCode::SupervisorData);
    write_word(regs_.ssp + 2, static_cast<uint16_t>(return_address >> 16), FunctionCode::SupervisorData);

    uint32_t high = read_word(vector * 4, FunctionCode::SupervisorData);
    uint32_t handler = high << 16 | read_word(vector * 4 + 2, FunctionCode::SupervisorData);

    // The queue is filled afresh at the handler, with 2 internal clocks
    // between its two words. PC keeps all 32 bits of the vector; only the
    // bus drops the top byte.
    regs_.ir = read_word(handler, FunctionCode::SupervisorProgram);
    idle(2);
    regs_.irc = read_word(handler + 2, FunctionCode::SupervisorProgram);
    regs_.pc = handler + 4;
}

void M68008::refill_queue(uint32_t address) {
    regs_.ir = read_word(address, program_space());
    regs_.irc = read_word(address + 2, program_space());
    regs_.pc = address + 4;
}

void M68008::instruction_exception(unsigned vector) {
    // Illegal instructions, lines A and F, and privilege violations: 4
    // internal clocks before the frame, and the return address is the
    // offending opcode's own, 4 behind PC. The instruction never ran, so it
    // is not traced (Programmer's Reference Manual, 6.3.8).
    trace_pending_ = false;
    idle(4);
    exception(vector, regs_.pc - 4);
}

bool M68008::supervisor_or_trap() {
    if (regs_.sr & SR_S) return true;
    // The check comes before any operand is touched.
    instruction_exception(VECTOR_PRIVILEGE);
    return false;
}

void M68008::step() {
    if (trace_pending_) {
        // The trace exception for the instruction before, returning to the
        // next, whose opcode is already in IR. On the chip it belongs to the
        // traced instruction; here it is a step of its own so that a step
        // ends where each SingleStepTests case does.
        trace_pending_ = false;
        idle(4);
        exception(VECTOR_TRACE, regs_.pc - 4);
        return;
    }
    if (stopped_) {
        // Nothing on the bus until an interrupt or reset: 4 clocks a step, a
        // bus cycle's worth, so that time still passes for a machine that
        // only calls step().
        idle(STOPPED_STEP_CLOCKS);
        return;
    }
    // T is looked at as the instruction starts: one that sets it is not
    // traced, one that clears it is. An instruction that is refused cancels
    // its trace (instruction_exception).
    trace_pending_ = (regs_.sr & SR_T) != 0;
    execute(regs_.ir);
}

void M68008::execute(uint16_t opcode) {
    switch (decode(opcode)) {
    case Instruction::Illegal: instruction_exception(VECTOR_ILLEGAL); return;
    case Instruction::LineA: instruction_exception(VECTOR_LINE_A); return;
    case Instruction::LineF: instruction_exception(VECTOR_LINE_F); return;
    case Instruction::NotYet: throw NotImplemented("M68008::step: opcode not implemented yet");
    case Instruction::Nop: prefetch(); return;
    case Instruction::Moveq: moveq(opcode); return;
    case Instruction::Move: move(opcode, move_size(opcode)); return;
    case Instruction::Movea: movea(opcode); return;
    case Instruction::Lea: lea(opcode); return;
    case Instruction::Pea: pea(opcode); return;
    case Instruction::Trap: trap(opcode); return;
    case Instruction::Trapv: trapv(); return;
    case Instruction::Chk: chk(opcode); return;
    case Instruction::Rte: rte(); return;
    case Instruction::Stop: stop(); return;
    case Instruction::Reset: reset_instruction(); return;
    case Instruction::MoveUsp: move_usp(opcode); return;
    case Instruction::LogicToCcr: logic_to_status(opcode, false); return;
    case Instruction::LogicToSr: logic_to_status(opcode, true); return;
    case Instruction::MoveToCcr: move_to_status(opcode, false); return;
    case Instruction::MoveToSr: move_to_status(opcode, true); return;
    case Instruction::MoveFromSr: move_from_sr(opcode); return;
    case Instruction::Bcc: bcc(opcode); return;
    case Instruction::Bsr: bsr(opcode); return;
    case Instruction::Dbcc: dbcc(opcode); return;
    case Instruction::Jmp: jmp(opcode); return;
    case Instruction::Jsr: jsr(opcode); return;
    case Instruction::Rts: rts(); return;
    case Instruction::Rtr: rtr(); return;
    case Instruction::Link: link(opcode); return;
    case Instruction::Unlk: unlk(opcode); return;
    case Instruction::AddSub: add_sub(opcode); return;
    case Instruction::AddqSubq: addq_subq(opcode); return;
    case Instruction::AddiSubi: addi_subi(opcode); return;
    case Instruction::AddaSuba: adda_suba(opcode); return;
    case Instruction::AddxSubx: addx_subx(opcode); return;
    case Instruction::NegNegxClr: neg_negx_clr(opcode); return;
    case Instruction::Tst: tst(opcode); return;
    case Instruction::Cmp: cmp(opcode); return;
    case Instruction::Cmpa: cmpa(opcode); return;
    case Instruction::Cmpi: cmpi(opcode); return;
    case Instruction::Cmpm: cmpm(opcode); return;
    case Instruction::Ext: ext(opcode); return;
    case Instruction::Swap: swap(opcode); return;
    case Instruction::Exg: exg(opcode); return;
    }
}

bool M68008::condition(unsigned cc) const {
    bool c = regs_.sr & SR_C;
    bool v = regs_.sr & SR_V;
    bool z = regs_.sr & SR_Z;
    bool n = regs_.sr & SR_N;
    switch (cc) {
    case 0x0: return true;
    case 0x1: return false;
    case 0x2: return !c && !z;          // HI
    case 0x3: return c || z;            // LS
    case 0x4: return !c;                // CC
    case 0x5: return c;                 // CS
    case 0x6: return !z;                // NE
    case 0x7: return z;                 // EQ
    case 0x8: return !v;                // VC
    case 0x9: return v;                 // VS
    case 0xA: return !n;                // PL
    case 0xB: return n;                 // MI
    case 0xC: return n == v;            // GE
    case 0xD: return n != v;            // LT
    case 0xE: return !z && n == v;      // GT
    default: return z || n != v;        // LE
    }
}

Size M68008::move_size(uint16_t opcode) {
    // MOVE's own size field, bits 13-12: 01 byte, 11 word, 10 long.
    switch (opcode & 0x3000) {
    case 0x1000: return Size::Byte;
    case 0x3000: return Size::Word;
    }
    return Size::Long;
}

void M68008::moveq(uint16_t opcode) {
    // MOVEQ #d8,Dn: 0111 rrr0 dddddddd, the byte sign-extended to 32 bits.
    // N and Z from the result, V and C cleared, X untouched.
    uint32_t value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(opcode & 0xFF)));
    regs_.d[(opcode >> 9) & 7] = value;
    set_move_flags(value, Size::Long);
    prefetch();
}

void M68008::movea(uint16_t opcode) {
    // MOVEA <ea>,An: a word is sign-extended to the whole register, and the
    // condition codes are left alone.
    Size size = move_size(opcode);
    uint32_t value = read(resolve((opcode >> 3) & 7, opcode & 7, size), size);
    if (size == Size::Word) value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
    address_register((opcode >> 9) & 7) = value;
    prefetch();
}

void M68008::lea(uint16_t opcode) {
    // LEA <ea>,An: 0100 rrr1 11 mmm xxx.
    unsigned mode = (opcode >> 3) & 7;
    uint32_t address = control_address(mode, opcode & 7);
    // (d8,An,Xn) and (d8,PC,Xn) spend 2 more clocks before the prefetch.
    if (mode == 6 || (mode == 7 && (opcode & 7) == 3)) idle(2);
    address_register((opcode >> 9) & 7) = address;
    prefetch();
}

void M68008::pea(uint16_t opcode) {
    // PEA <ea>: 0100 1000 01 mmm xxx.
    unsigned mode = (opcode >> 3) & 7;
    uint32_t address = control_address(mode, opcode & 7);
    if (mode == 6 || (mode == 7 && (opcode & 7) == 3)) idle(2);
    // The absolute modes push before the prefetch; the others after
    // (SingleStepTests' bus order, the 68000's microcode).
    if (mode == 7 && (opcode & 7) <= 1) {
        push_long(address);
        prefetch();
    } else {
        prefetch();
        push_long(address);
    }
}

void M68008::trap(uint16_t opcode) {
    // TRAP #n: 0100 1110 0100 nnnn. The return address is the next
    // instruction's, two bytes on from the opcode.
    idle(4);
    exception(VECTOR_TRAP_0 + (opcode & 0xF), regs_.pc - 2);
}

void M68008::trapv() {
    // TRAPV: with V clear, a prefetch and nothing more. With V set, the same
    // read goes out but already as a supervisor program access, even from the
    // user state, and its word is thrown away by the exception that follows,
    // which returns to the next instruction (SingleStepTests).
    if (!(regs_.sr & SR_V)) {
        prefetch();
        return;
    }
    read_word(regs_.pc, FunctionCode::SupervisorProgram);
    exception(VECTOR_TRAPV, regs_.pc - 2);
}

void M68008::chk(uint16_t opcode) {
    // CHK <ea>,Dn: 0100 rrr1 10 mmm xxx, word-sized on the 68000. Traps if Dn
    // is above the bound or below zero. The manual defines N only when it
    // traps and leaves Z, V and C undefined; the 68000 sets N and Z from Dn
    // and clears V and C (SingleStepTests).
    int16_t bound = static_cast<int16_t>(read(resolve((opcode >> 3) & 7, opcode & 7, Size::Word), Size::Word));
    int16_t value = static_cast<int16_t>(regs_.d[(opcode >> 9) & 7]);
    uint16_t ccr = regs_.sr & SR_X;
    if (value < 0) ccr |= SR_N;
    if (value == 0) ccr |= SR_Z;
    regs_.sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | ccr);
    if (value >= 0 && value <= bound) {
        idle(6);
        prefetch();
        return;
    }
    // Trapping takes 8 clocks or 10. Above the bound with Dn not negative it
    // is 8. With Dn negative the trap is certain, and the time follows the
    // sign of the 16-bit Dn - bound with its overflow ignored: 8 if that is
    // above zero, 10 if not. Worked out from every register-bound case in
    // SingleStepTests' CHK file; the manual gives a single time for the trap.
    int16_t difference = static_cast<int16_t>(value - bound);
    idle(value >= 0 || difference > 0 ? 8 : 10);
    exception(VECTOR_CHK, regs_.pc - 2);
}

void M68008::rte() {
    // RTE: privileged. SR, then PC's high and low words, all read from the
    // supervisor stack before the new SR takes effect; the queue is then
    // filled at the return address under the new SR's function codes
    // (SingleStepTests).
    if (!supervisor_or_trap()) return;
    uint16_t sr = read_word(regs_.ssp, FunctionCode::SupervisorData);
    uint32_t high = read_word(regs_.ssp + 2, FunctionCode::SupervisorData);
    uint32_t pc = high << 16 | read_word(regs_.ssp + 4, FunctionCode::SupervisorData);
    regs_.ssp += 6;
    regs_.sr = static_cast<uint16_t>(sr & SR_IMPLEMENTED);
    refill_queue(pc);
}

void M68008::stop() {
    // STOP #imm: privileged. SR takes the immediate, which is already in IRC,
    // and the processor stops with the queue as it is and PC past the
    // immediate, which is where an interrupt will return to.
    if (!supervisor_or_trap()) return;
    regs_.sr = static_cast<uint16_t>(regs_.irc & SR_IMPLEMENTED);
    idle(4);
    stopped_ = true;
}

void M68008::reset_instruction() {
    // RESET: privileged. The 68000 asserts its RESET output for 124 clocks to
    // reset the devices on the bus, and the CPU itself carries on (User's
    // Manual, RESET; 128 internal clocks in SingleStepTests). Nothing on the
    // QL is known to need the pulse yet, so the Bus is not told.
    if (!supervisor_or_trap()) return;
    idle(128);
    prefetch();
}

void M68008::move_usp(uint16_t opcode) {
    // MOVE An,USP (0100 1110 0110 0rrr) and MOVE USP,An (0110 1rrr).
    // Privileged. A7 here is the supervisor stack pointer.
    if (!supervisor_or_trap()) return;
    if (opcode & 8) {
        address_register(opcode & 7) = regs_.usp;
    } else {
        regs_.usp = address_register(opcode & 7);
    }
    prefetch();
}

void M68008::logic_to_status(uint16_t opcode, bool to_sr) {
    // ORI, ANDI and EORI to CCR (0000 0000/0010/1010 0011 1100) and to SR
    // (... 0111 1100), the immediate in the word that follows; to SR is
    // privileged. CCR takes only the immediate's low five bits' worth.
    if (to_sr && !supervisor_or_trap()) return;
    uint16_t immediate = take_extension();
    uint16_t sr = regs_.sr;
    switch (opcode & 0x0E00) {
    case 0x0000: sr |= immediate; break;
    case 0x0200: sr &= to_sr ? immediate : (immediate | ~SR_CCR); break;
    default: sr ^= immediate; break;
    }
    if (!to_sr) sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | (sr & SR_CCR));
    regs_.sr = static_cast<uint16_t>(sr & SR_IMPLEMENTED);
    // After a change to SR the queue is thrown away and fetched again, under
    // the new function codes (SingleStepTests).
    idle(8);
    refill_queue(regs_.pc - 2);
}

void M68008::move_to_status(uint16_t opcode, bool to_sr) {
    // MOVE <ea>,CCR (0100 0100 11 mmm xxx) and MOVE <ea>,SR (0100 0110 11
    // ...), from a word; to SR is privileged, and is checked before the
    // operand is read.
    if (to_sr && !supervisor_or_trap()) return;
    uint16_t value = static_cast<uint16_t>(read(resolve((opcode >> 3) & 7, opcode & 7, Size::Word), Size::Word));
    if (!to_sr) value = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | (value & SR_CCR));
    regs_.sr = static_cast<uint16_t>(value & SR_IMPLEMENTED);
    idle(4);
    refill_queue(regs_.pc - 2);
}

void M68008::move_from_sr(uint16_t opcode) {
    // MOVE SR,<ea>: 0100 0000 11 mmm xxx. Not privileged on the 68000 (it is
    // from the 68010 on).
    unsigned mode = (opcode >> 3) & 7;
    if (mode == 0) {
        regs_.d[opcode & 7] = (regs_.d[opcode & 7] & 0xFFFF0000) | regs_.sr;
        prefetch();
        idle(2);
        return;
    }
    // To memory the 68000 reads the destination before it writes it, and
    // throws the value away (SingleStepTests; the User's Manual warns of the
    // read).
    Operand dest = resolve(mode, opcode & 7, Size::Word);
    read(dest, Size::Word);
    prefetch();
    write(dest, Size::Word, regs_.sr);
}

uint32_t M68008::branch_target(uint16_t opcode) const {
    // Bcc, BRA and BSR: 0110 cccc dddddddd. A displacement byte of zero
    // means a word displacement follows, already in IRC. Either is relative
    // to the address after the opcode, 2 behind PC.
    int32_t displacement = static_cast<int8_t>(opcode & 0xFF);
    if (displacement == 0) displacement = static_cast<int16_t>(regs_.irc);
    return regs_.pc - 2 + static_cast<uint32_t>(displacement);
}

void M68008::bcc(uint16_t opcode) {
    // Taken: 2 internal clocks, and the queue filled afresh at the target.
    // Not taken: 4, and on past the displacement word if there is one
    // (SingleStepTests).
    if (condition((opcode >> 8) & 0xF)) {
        uint32_t target = branch_target(opcode);
        idle(2);
        refill_queue(target);
        return;
    }
    idle(4);
    if ((opcode & 0xFF) == 0) take_extension();
    prefetch();
}

void M68008::bsr(uint16_t opcode) {
    // BSR: 2 internal clocks, the return address pushed high word first --
    // past the displacement word if there is one -- and the queue filled at
    // the target (SingleStepTests).
    uint32_t target = branch_target(opcode);
    uint32_t return_address = (opcode & 0xFF) == 0 ? regs_.pc : regs_.pc - 2;
    idle(2);
    push_long(return_address);
    refill_queue(target);
}

void M68008::dbcc(uint16_t opcode) {
    // DBcc Dn,d16: 0101 cccc 1100 1rrr. With the condition true, 4 internal
    // clocks and on past the displacement word. Otherwise the low word of Dn
    // counts down, and unless it has run out to -1 the branch is taken: 2
    // internal clocks and the queue filled at the target (SingleStepTests).
    if (condition((opcode >> 8) & 0xF)) {
        idle(4);
        take_extension();
        prefetch();
        return;
    }
    uint32_t& dn = regs_.d[opcode & 7];
    uint16_t count = static_cast<uint16_t>(dn - 1);
    dn = (dn & 0xFFFF0000) | count;
    uint32_t target = regs_.pc - 2 + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(regs_.irc)));
    idle(2);
    if (count != 0xFFFF) {
        refill_queue(target);
        return;
    }
    // Run out: the manual gives 14(3/0) on the 68000, so three words are
    // read. The first is taken to be at the branch target, fetched before the
    // count is seen to have run out and thrown away; then the queue is filled
    // past the displacement word. SingleStepTests has no case to confirm the
    // order (m68008_flow_tests checks the time and the result).
    read_word(target, program_space());
    refill_queue(regs_.pc);
}

uint32_t M68008::jump_target(unsigned mode, unsigned reg, uint32_t& next) {
    // The extension words are used where they sit: the first is in IRC, and
    // (xxx).L's second is read from PC without moving it. The internal clocks
    // are SingleStepTests': 2 for a displacement or a short address, 6 with
    // an index, none for (An) or a long address.
    uint16_t extension = regs_.irc;
    int32_t displacement = static_cast<int16_t>(extension);
    next = regs_.pc;
    switch (mode) {
    case 2:
        next = regs_.pc - 2;
        return address_register(reg);
    case 5:
        idle(2);
        return address_register(reg) + static_cast<uint32_t>(displacement);
    case 6:
        idle(6);
        return indexed(address_register(reg), extension);
    }
    switch (reg) {
    case 0:
        idle(2);
        return static_cast<uint32_t>(displacement);
    case 1: {
        uint32_t low = read_word(regs_.pc, program_space());
        next = regs_.pc + 2;
        return static_cast<uint32_t>(extension) << 16 | low;
    }
    case 2:
        idle(2);
        return regs_.pc - 2 + static_cast<uint32_t>(displacement);
    default:
        idle(6);
        return indexed(regs_.pc - 2, extension);
    }
}

void M68008::jmp(uint16_t opcode) {
    // JMP <ea>: 0100 1110 11 mmm xxx, control modes.
    uint32_t next;
    uint32_t target = jump_target((opcode >> 3) & 7, opcode & 7, next);
    refill_queue(target);
}

void M68008::jsr(uint16_t opcode) {
    // JSR <ea>: 0100 1110 10 mmm xxx, control modes. The return address is
    // pushed, high word first, between the two reads that fill the queue at
    // the target (SingleStepTests).
    uint32_t next;
    uint32_t target = jump_target((opcode >> 3) & 7, opcode & 7, next);
    regs_.ir = read_word(target, program_space());
    push_long(next);
    regs_.irc = read_word(target + 2, program_space());
    regs_.pc = target + 4;
}

uint32_t M68008::pop_long() {
    uint32_t& sp = address_register(7);
    uint32_t high = read_word(sp, data_space());
    uint32_t value = high << 16 | read_word(sp + 2, data_space());
    sp += 4;
    return value;
}

void M68008::rts() {
    // RTS: the return address popped, and the queue filled there.
    refill_queue(pop_long());
}

void M68008::rtr() {
    // RTR: CCR popped from a word -- the system byte is left alone -- then
    // the return address, and the queue filled there.
    uint32_t& sp = address_register(7);
    uint16_t ccr = read_word(sp, data_space());
    sp += 2;
    regs_.sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | (ccr & SR_CCR));
    refill_queue(pop_long());
}

void M68008::link(uint16_t opcode) {
    // LINK An,#d16: 0100 1110 0101 0rrr. An is pushed, takes the stack
    // pointer, and the displacement is added to the stack pointer. The
    // displacement is taken first, then the push, then the prefetch
    // (SingleStepTests).
    unsigned n = opcode & 7;
    int32_t displacement = static_cast<int16_t>(take_extension());
    push_long(address_register(n));
    uint32_t& sp = address_register(7);
    address_register(n) = sp;
    sp += static_cast<uint32_t>(displacement);
    prefetch();
}

void M68008::unlk(uint16_t opcode) {
    // UNLK An: 0100 1110 0101 1rrr. The stack pointer takes An, and An is
    // popped.
    unsigned n = opcode & 7;
    address_register(7) = address_register(n);
    uint32_t value = pop_long();
    address_register(n) = value;
    prefetch();
}

Size M68008::size_field(uint16_t opcode) {
    switch (opcode & 0x00C0) {
    case 0x0000: return Size::Byte;
    case 0x0040: return Size::Word;
    }
    return Size::Long;
}

uint32_t M68008::arithmetic(uint32_t dst, uint32_t src, Size size, bool subtract, bool extend, bool affects_x) {
    uint32_t mask = size_mask(size);
    uint32_t sign = (mask >> 1) + 1;
    dst &= mask;
    src &= mask;
    uint32_t x = extend && (regs_.sr & SR_X) ? 1 : 0;

    // Carry and borrow from the sum worked in 64 bits; overflow from the
    // signs: an add overflows when both operands have the same sign and the
    // result the other, a subtract when the operands differ and the result
    // takes the subtrahend's sign.
    uint64_t wide = subtract ? uint64_t{dst} - src - x : uint64_t{dst} + src + x;
    uint32_t result = static_cast<uint32_t>(wide) & mask;
    bool carry = (wide >> (static_cast<unsigned>(size) * 8)) & 1;
    bool overflow = subtract ? ((src ^ dst) & (result ^ dst) & sign) != 0 : (~(src ^ dst) & (result ^ dst) & sign) != 0;

    uint16_t ccr = regs_.sr & (affects_x ? 0 : SR_X);
    if (affects_x && carry) ccr |= SR_X;
    if (result & sign) ccr |= SR_N;
    bool zero = result == 0;
    if (extend) zero = zero && (regs_.sr & SR_Z);
    if (zero) ccr |= SR_Z;
    if (overflow) ccr |= SR_V;
    if (carry) ccr |= SR_C;
    regs_.sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | ccr);
    return result;
}

void M68008::finish_read_modify_write(const Operand& dest, Size size, uint32_t value, unsigned long_register_clocks) {
    prefetch();
    if (dest.kind != Operand::Kind::Memory) {
        write(dest, size, value);
        if (size == Size::Long) idle(long_register_clocks);
        return;
    }
    if (size == Size::Long) {
        write_word(dest.address + 2, static_cast<uint16_t>(value), data_space());
        write_word(dest.address, static_cast<uint16_t>(value >> 16), data_space());
        return;
    }
    write(dest, size, value);
}

void M68008::add_sub(uint16_t opcode) {
    // ADD and SUB: 1101 / 1001 rrr d ss mmm xxx. With d clear, <ea> to Dn;
    // set, Dn to a memory operand.
    bool subtract = (opcode >> 12) == 0x9;
    Size size = size_field(opcode);
    Operand dn{Operand::Kind::DataRegister, static_cast<unsigned>((opcode >> 9) & 7), 0, data_space(), 0};
    Operand ea = resolve((opcode >> 3) & 7, opcode & 7, size);
    uint32_t ea_value = read(ea, size);
    if (opcode & 0x0100) {
        uint32_t result = arithmetic(ea_value, read(dn, size), size, subtract, false, true);
        finish_read_modify_write(ea, size, result, 0);
    } else {
        uint32_t result = arithmetic(read(dn, size), ea_value, size, subtract, false, true);
        finish_read_modify_write(dn, size, result, ea.kind == Operand::Kind::Memory ? 2 : 4);
    }
}

void M68008::addq_subq(uint16_t opcode) {
    // ADDQ and SUBQ #1-8: 0101 ddd s ss mmm xxx, 0 standing for 8. To An the
    // whole register changes whatever the size and the flags do not.
    bool subtract = opcode & 0x0100;
    uint32_t data = (opcode >> 9) & 7;
    if (data == 0) data = 8;
    Size size = size_field(opcode);
    unsigned mode = (opcode >> 3) & 7;
    if (mode == 1) {
        uint32_t& an = address_register(opcode & 7);
        an = subtract ? an - data : an + data;
        prefetch();
        idle(4);
        return;
    }
    Operand dest = resolve(mode, opcode & 7, size);
    uint32_t result = arithmetic(read(dest, size), data, size, subtract, false, true);
    finish_read_modify_write(dest, size, result, 4);
}

void M68008::addi_subi(uint16_t opcode) {
    // ADDI and SUBI #imm,<ea>: 0000 0110 / 0100 ss mmm xxx, the immediate
    // taken before the destination's extension words.
    bool subtract = (opcode & 0x0E00) == 0x0400;
    Size size = size_field(opcode);
    uint32_t immediate = read(resolve(7, 4, size), size);
    Operand dest = resolve((opcode >> 3) & 7, opcode & 7, size);
    uint32_t result = arithmetic(read(dest, size), immediate, size, subtract, false, true);
    finish_read_modify_write(dest, size, result, 4);
}

void M68008::adda_suba(uint16_t opcode) {
    // ADDA and SUBA <ea>,An: 1101 / 1001 rrr s11 mmm xxx, s set for long. A
    // word is sign-extended, the whole of An changes, and the flags do not.
    // 4 internal clocks after the prefetch, but 2 for a long read from
    // memory (SingleStepTests).
    bool subtract = (opcode >> 12) == 0x9;
    Size size = (opcode & 0x0100) ? Size::Long : Size::Word;
    Operand source = resolve((opcode >> 3) & 7, opcode & 7, size);
    uint32_t value = read(source, size);
    if (size == Size::Word) value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
    uint32_t& an = address_register((opcode >> 9) & 7);
    an = subtract ? an - value : an + value;
    prefetch();
    idle(size == Size::Long && source.kind == Operand::Kind::Memory ? 2 : 4);
}

uint32_t M68008::read_predecrement_low_first(unsigned reg, Size size, uint32_t& address) {
    uint32_t& an = address_register(reg);
    an -= step_size(reg, size);
    address = an;
    if (size != Size::Long) return read_memory(address, size, data_space());
    uint32_t low = read_word(address + 2, data_space());
    return static_cast<uint32_t>(read_word(address, data_space())) << 16 | low;
}

void M68008::addx_subx(uint16_t opcode) {
    // ADDX and SUBX: 1101 / 1001 xxx1 ss00 myyy, Dy to Dx with m clear, or
    // -(Ay) to -(Ax) with it set. X joins the sum, and Z is only ever cleared.
    bool subtract = (opcode >> 12) == 0x9;
    Size size = size_field(opcode);
    unsigned rx = (opcode >> 9) & 7;
    unsigned ry = opcode & 7;
    if (!(opcode & 0x0008)) {
        Operand dx{Operand::Kind::DataRegister, rx, 0, data_space(), 0};
        uint32_t result = arithmetic(regs_.d[rx], regs_.d[ry], size, subtract, true, true);
        finish_read_modify_write(dx, size, result, 4);
        return;
    }
    // In memory: 2 internal clocks, the source and then the destination
    // read, a long low word first, then the result written low word first
    // with the prefetch between a long's two words (SingleStepTests).
    idle(2);
    uint32_t address;
    uint32_t source = read_predecrement_low_first(ry, size, address);
    uint32_t destination = read_predecrement_low_first(rx, size, address);
    uint32_t result = arithmetic(destination, source, size, subtract, true, true);
    if (size == Size::Long) {
        write_word(address + 2, static_cast<uint16_t>(result), data_space());
        prefetch();
        write_word(address, static_cast<uint16_t>(result >> 16), data_space());
        return;
    }
    prefetch();
    write_memory(address, size, result);
}

void M68008::neg_negx_clr(uint16_t opcode) {
    // NEGX, CLR and NEG <ea>: 0100 0000 / 0010 / 0100 ss mmm xxx. All three
    // read the operand before writing it -- CLR too, and throws the value
    // away (SingleStepTests; the User's Manual warns of CLR's read). A long
    // in Dn costs 2 internal clocks.
    Size size = size_field(opcode);
    Operand dest = resolve((opcode >> 3) & 7, opcode & 7, size);
    uint32_t value = read(dest, size);
    uint32_t result;
    switch (opcode & 0x0F00) {
    case 0x0000:
        result = arithmetic(0, value, size, true, true, true);
        break;
    case 0x0200:
        // CLR: Z set, N, V and C cleared, X untouched.
        result = 0;
        regs_.sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | (regs_.sr & SR_X) | SR_Z);
        break;
    default:
        result = arithmetic(0, value, size, true, false, true);
        break;
    }
    finish_read_modify_write(dest, size, result, 2);
}

void M68008::tst(uint16_t opcode) {
    // TST <ea>: 0100 1010 ss mmm xxx. N and Z from the operand, V and C
    // cleared, X untouched.
    Size size = size_field(opcode);
    set_move_flags(read(resolve((opcode >> 3) & 7, opcode & 7, size), size), size);
    prefetch();
}

// The compares subtract without storing the result and leave X alone. A long
// compare with a register spends 2 internal clocks after the prefetch, from
// whatever source; with memory it spends none (SingleStepTests).

void M68008::cmp(uint16_t opcode) {
    // CMP <ea>,Dn: 1011 rrr0 ss mmm xxx.
    Size size = size_field(opcode);
    uint32_t source = read(resolve((opcode >> 3) & 7, opcode & 7, size), size);
    arithmetic(regs_.d[(opcode >> 9) & 7], source, size, true, false, false);
    prefetch();
    if (size == Size::Long) idle(2);
}

void M68008::cmpa(uint16_t opcode) {
    // CMPA <ea>,An: 1011 rrr s11 mmm xxx, s set for long. A word is
    // sign-extended and compared with the whole of An; 2 internal clocks
    // either way.
    Size size = (opcode & 0x0100) ? Size::Long : Size::Word;
    uint32_t source = read(resolve((opcode >> 3) & 7, opcode & 7, size), size);
    if (size == Size::Word) source = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(source)));
    arithmetic(address_register((opcode >> 9) & 7), source, Size::Long, true, false, false);
    prefetch();
    idle(2);
}

void M68008::cmpi(uint16_t opcode) {
    // CMPI #imm,<ea>: 0000 1100 ss mmm xxx, the immediate taken first.
    Size size = size_field(opcode);
    uint32_t immediate = read(resolve(7, 4, size), size);
    Operand dest = resolve((opcode >> 3) & 7, opcode & 7, size);
    arithmetic(read(dest, size), immediate, size, true, false, false);
    prefetch();
    if (size == Size::Long && dest.kind == Operand::Kind::DataRegister) idle(2);
}

void M68008::cmpm(uint16_t opcode) {
    // CMPM (Ay)+,(Ax)+: 1011 xxx1 ss00 1yyy, the source read first.
    Size size = size_field(opcode);
    uint32_t source = read(resolve(3, opcode & 7, size), size);
    uint32_t destination = read(resolve(3, (opcode >> 9) & 7, size), size);
    arithmetic(destination, source, size, true, false, false);
    prefetch();
}

void M68008::ext(uint16_t opcode) {
    // EXT.W Dn (0100 1000 1000 0rrr) sign-extends the low byte to a word,
    // EXT.L (... 1100 0rrr) the low word to a long. Flags as for a move.
    uint32_t& dn = regs_.d[opcode & 7];
    if (opcode & 0x0040) {
        dn = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(dn)));
        set_move_flags(dn, Size::Long);
    } else {
        uint16_t word = static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(dn)));
        dn = (dn & 0xFFFF0000) | word;
        set_move_flags(word, Size::Word);
    }
    prefetch();
}

void M68008::swap(uint16_t opcode) {
    // SWAP Dn: 0100 1000 0100 0rrr. The halves change places; flags from
    // the whole long, as for a move.
    uint32_t& dn = regs_.d[opcode & 7];
    dn = dn << 16 | dn >> 16;
    set_move_flags(dn, Size::Long);
    prefetch();
}

void M68008::exg(uint16_t opcode) {
    // EXG: 1100 xxx1 ooooo yyy, opmode 01000 for two data registers, 01001
    // for two address registers, 10001 for Dx and Ay. 2 internal clocks
    // after the prefetch (SingleStepTests).
    unsigned x = (opcode >> 9) & 7;
    unsigned y = opcode & 7;
    uint32_t& rx = (opcode & 0x01F8) == 0x0148 ? address_register(x) : regs_.d[x];
    uint32_t& ry = (opcode & 0x01F8) == 0x0140 ? regs_.d[y] : address_register(y);
    uint32_t held = rx;
    rx = ry;
    ry = held;
    prefetch();
    idle(2);
}

} // namespace ql
