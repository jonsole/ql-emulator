#include "m68008.h"
#include "m68008_decode.h"
#include "m68008_internal.h"

#include <stdexcept>

namespace ql {

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

uint32_t M68008::pop_long() {
    uint32_t& sp = address_register(7);
    uint32_t high = read_word(sp, data_space());
    uint32_t value = high << 16 | read_word(sp + 2, data_space());
    sp += 4;
    return value;
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

Size M68008::size_field(uint16_t opcode) {
    switch (opcode & 0x00C0) {
    case 0x0000: return Size::Byte;
    case 0x0040: return Size::Word;
    }
    return Size::Long;
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

} // namespace ql
