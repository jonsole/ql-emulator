#include "m68008.h"

#include <stdexcept>

namespace ql {

namespace {

/// The shortest 68008 bus cycle, states S0-S7 with no wait states: the
/// 68008's read and write cycle timing in the M68000 User's Manual.
constexpr unsigned BYTE_CYCLE_CLOCKS = 4;

/// The 68000 family drives A1-A23; with A0 that is 24 bits. See m68008.h.
constexpr uint32_t ADDRESS_MASK = 0x00FFFFFF;

// SR's bits: the system byte's supervisor bit, and the condition codes.
constexpr uint16_t SR_S = 0x2000;
constexpr uint16_t SR_X = 0x0010;
constexpr uint16_t SR_N = 0x0008;
constexpr uint16_t SR_Z = 0x0004;
constexpr uint16_t SR_CCR = 0x001F;

/// The modes that name a place without reading or writing it: (An),
/// (d16,An), (d8,An,Xn), (xxx).W, (xxx).L, (d16,PC), (d8,PC,Xn).
bool is_control_mode(unsigned mode, unsigned reg) {
    return mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3);
}

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

void M68008::step() {
    uint16_t opcode = regs_.ir;
    if (opcode == 0x4E71) {
        // NOP: nothing but the prefetch that every instruction ends with.
        prefetch();
        return;
    }
    if ((opcode & 0xF100) == 0x7000) {
        // MOVEQ #d8,Dn: 0111 rrr0 dddddddd, the byte sign-extended to 32 bits.
        // N and Z from the result, V and C cleared, X untouched.
        uint32_t value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(opcode & 0xFF)));
        regs_.d[(opcode >> 9) & 7] = value;
        uint16_t ccr = regs_.sr & SR_X;
        if (value & 0x80000000) ccr |= SR_N;
        if (value == 0) ccr |= SR_Z;
        regs_.sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | ccr);
        prefetch();
        return;
    }
    if ((opcode & 0xF1C0) == 0x41C0) {
        // LEA <ea>,An: 0100 rrr1 11 mmm xxx, control modes only.
        uint32_t address = control_address((opcode >> 3) & 7, opcode & 7);
        // (d8,An,Xn) and (d8,PC,Xn) spend 2 more clocks before the prefetch.
        unsigned mode = (opcode >> 3) & 7;
        if (mode == 6 || (mode == 7 && (opcode & 7) == 3)) idle(2);
        address_register((opcode >> 9) & 7) = address;
        prefetch();
        return;
    }
    if ((opcode & 0xFFC0) == 0x4840 && is_control_mode((opcode >> 3) & 7, opcode & 7)) {
        // PEA <ea>: 0100 1000 01 mmm xxx. Mode 0 of the same pattern is SWAP.
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
        return;
    }
    if ((opcode & 0xC000) == 0 && (opcode & 0x3000) != 0) {
        // MOVE <ea>,<ea>: 00ss DDD ddd sss SSS, with ss 01 byte, 11 word, 10
        // long. A destination mode of 1 is MOVEA. Not every mode is allowed:
        // no byte from An, nothing past #imm as a source, no PC-relative or
        // #imm destination.
        Size size = (opcode & 0x3000) == 0x1000 ? Size::Byte : (opcode & 0x3000) == 0x3000 ? Size::Word : Size::Long;
        unsigned source_mode = (opcode >> 3) & 7;
        unsigned dest_mode = (opcode >> 6) & 7;
        bool source_ok = source_mode != 7 || (opcode & 7) <= 4;
        if (size == Size::Byte && source_mode == 1) source_ok = false;
        bool dest_ok = dest_mode != 1 && (dest_mode != 7 || ((opcode >> 9) & 7) <= 1);
        if (source_ok && dest_ok) {
            move(opcode, size);
            return;
        }
        if (source_ok && dest_mode == 1 && size != Size::Byte) {
            // MOVEA <ea>,An: a word is sign-extended to the whole register,
            // and the condition codes are left alone.
            uint32_t value = read(resolve(source_mode, opcode & 7, size), size);
            if (size == Size::Word) value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
            address_register((opcode >> 9) & 7) = value;
            prefetch();
            return;
        }
    }
    // Every opcode will decode by the end of Stage 1 -- the ones the 68000
    // does not define take the illegal-instruction, line A or line F
    // exceptions (ticket #3). Until then, an opcode with no implementation is
    // a gap in the core, and saying so beats running on in the wrong state.
    throw std::logic_error("M68008::step: opcode not implemented yet");
}

} // namespace ql
