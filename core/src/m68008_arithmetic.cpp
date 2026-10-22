// The 68008 core: integer arithmetic -- ADD, SUB, ADDA, SUBA, ADDQ, SUBQ,
// ADDI, SUBI, ADDX, SUBX, NEG, NEGX, CMP, CMPA, CMPI, CMPM, CLR, TST, EXT,
// SWAP and EXG.

#include "m68008_internal.h"

namespace ql {

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
    address = an - step_size(reg, size);
    if (size != Size::Long) {
        an = address;
        return read_memory(address, size, data_space());
    }
    // A long's decrement is made only once both words are read: an odd An
    // faults on the low word with An as it was (SingleStepTests).
    uint32_t low = read_word(address + 2, data_space());
    uint32_t value = static_cast<uint32_t>(read_word(address, data_space())) << 16 | low;
    an = address;
    return value;
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
    fault_pc_ += 2;
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
    // CMPM (Ay)+,(Ax)+: 1011 xxx1 ss00 1yyy, the source read first. Like
    // -(An), it moves the fault PC on by 2 (SingleStepTests).
    Size size = size_field(opcode);
    fault_pc_ += 2;
    // Ay moves on by a word before its read even for a long, and Ax only
    // after its read: a fault on the source leaves Ay 2 on, one on the
    // destination leaves Ax as it was (SingleStepTests).
    Operand source_operand = resolve(3, opcode & 7, size);
    if (size == Size::Long) undo_postincrement_on_fault(opcode & 7, source_operand.address, 2);
    uint32_t source = read(source_operand, size);
    Operand destination_operand = resolve(3, (opcode >> 9) & 7, size);
    undo_postincrement_on_fault((opcode >> 9) & 7, destination_operand.address, step_size((opcode >> 9) & 7, size));
    uint32_t destination = read(destination_operand, size);
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
