// The 68008 core: logic and bits -- AND, OR, EOR and NOT, with ANDI, ORI and
// EORI; BTST, BCHG, BCLR and BSET; Scc and TAS.
//
// AND, OR, EOR and NOT set N and Z from the result, clear V and C, and leave X
// alone, as a move does. Their bus order and internal clocks are ADD's, and
// NOT's are NEG's (SingleStepTests).

#include "m68008_internal.h"

namespace ql {

void M68008::and_or(uint16_t opcode) {
    // AND and OR: 1100 / 1000 rrr d ss mmm xxx. With d clear, <ea> to Dn;
    // set, Dn to a memory operand.
    bool is_and = (opcode >> 12) == 0xC;
    Size size = size_field(opcode);
    Operand dn{Operand::Kind::DataRegister, static_cast<unsigned>((opcode >> 9) & 7), 0, data_space(), 0};
    Operand ea = resolve((opcode >> 3) & 7, opcode & 7, size);
    uint32_t value = read(ea, size);
    uint32_t result = is_and ? value & read(dn, size) : value | read(dn, size);
    set_move_flags(result, size);
    if (opcode & 0x0100) {
        finish_read_modify_write(ea, size, result, 0);
    } else {
        finish_read_modify_write(dn, size, result, ea.kind == Operand::Kind::Memory ? 2 : 4);
    }
}

void M68008::eor(uint16_t opcode) {
    // EOR Dn,<ea>: 1011 rrr1 ss mmm xxx. Only from Dn; the destination can be
    // a data register too.
    Size size = size_field(opcode);
    Operand dest = resolve((opcode >> 3) & 7, opcode & 7, size);
    uint32_t result = read(dest, size) ^ (regs_.d[(opcode >> 9) & 7] & size_mask(size));
    set_move_flags(result, size);
    finish_read_modify_write(dest, size, result, 4);
}

void M68008::logic_immediate(uint16_t opcode) {
    // ORI, ANDI and EORI #imm,<ea>: 0000 0000 / 0010 / 1010 ss mmm xxx, the
    // immediate taken before the destination's extension words.
    Size size = size_field(opcode);
    uint32_t immediate = read(resolve(7, 4, size), size);
    Operand dest = resolve((opcode >> 3) & 7, opcode & 7, size);
    uint32_t value = read(dest, size);
    uint32_t result;
    switch (opcode & 0x0E00) {
    case 0x0000: result = value | immediate; break;
    case 0x0200: result = value & immediate; break;
    default: result = value ^ immediate; break;
    }
    set_move_flags(result, size);
    finish_read_modify_write(dest, size, result, 4);
}

void M68008::logical_not(uint16_t opcode) {
    // NOT <ea>: 0100 0110 ss mmm xxx.
    Size size = size_field(opcode);
    Operand dest = resolve((opcode >> 3) & 7, opcode & 7, size);
    uint32_t result = ~read(dest, size) & size_mask(size);
    set_move_flags(result, size);
    finish_read_modify_write(dest, size, result, 2);
}

void M68008::bit_operation(uint16_t opcode) {
    // BTST, BCHG, BCLR and BSET (bits 7-6: 00, 01, 10, 11), with the bit
    // number in Dn (0000 rrr1 tt mmm xxx) or in the low byte of an extension
    // word taken first (0000 1000 tt mmm xxx). On a data register the bit is
    // numbered modulo 32; in memory the operand is a byte and the bit modulo
    // 8. Z is set if the bit was clear; nothing else changes.
    unsigned type = (opcode >> 6) & 3;
    uint32_t number = (opcode & 0x0100) ? regs_.d[(opcode >> 9) & 7] : take_extension();
    unsigned mode = (opcode >> 3) & 7;
    Size size = mode == 0 ? Size::Long : Size::Byte;
    uint32_t bit = 1u << (number & (mode == 0 ? 31 : 7));

    Operand dest = resolve(mode, opcode & 7, size);
    uint32_t value = read(dest, size);
    regs_.sr = static_cast<uint16_t>((value & bit) ? regs_.sr & ~SR_Z : regs_.sr | SR_Z);
    if (type == 0) {
        // BTST: 2 internal clocks after the prefetch on Dn or an immediate.
        prefetch();
        if (dest.kind != Operand::Kind::Memory) idle(2);
        return;
    }
    uint32_t result = type == 1 ? value ^ bit : type == 2 ? value & ~bit : value | bit;
    prefetch();
    write(dest, size, result);
    if (dest.kind == Operand::Kind::DataRegister) {
        // On Dn the time depends on the bit: BCHG and BSET spend 2 internal
        // clocks below bit 16 and 4 from it, BCLR 4 and 6 (SingleStepTests).
        unsigned clocks = type == 2 ? 4 : 2;
        if ((number & 31) >= 16) clocks += 2;
        idle(clocks);
    }
}

void M68008::scc(uint16_t opcode) {
    // Scc <ea>: 0101 cccc 11 mmm xxx. A byte of all ones if the condition
    // holds, all zeros if not. Memory is read first and the value thrown
    // away; on Dn a true condition costs 2 internal clocks (SingleStepTests).
    bool holds = condition((opcode >> 8) & 0xF);
    Operand dest = resolve((opcode >> 3) & 7, opcode & 7, Size::Byte);
    if (dest.kind == Operand::Kind::Memory) read(dest, Size::Byte);
    prefetch();
    write(dest, Size::Byte, holds ? 0xFF : 0x00);
    if (dest.kind == Operand::Kind::DataRegister && holds) idle(2);
}

void M68008::tas(uint16_t opcode) {
    // TAS <ea>: 0100 1010 11 mmm xxx. N and Z from the byte, V and C
    // cleared, then its top bit set. In memory the 68000 makes this one
    // indivisible read-modify-write cycle; here it is a read, 2 internal
    // clocks and a write, then the prefetch -- the same cycles in the same
    // order (SingleStepTests), and nothing on the QL can tell the
    // difference.
    Operand dest = resolve((opcode >> 3) & 7, opcode & 7, Size::Byte);
    uint32_t value = read(dest, Size::Byte);
    set_move_flags(value, Size::Byte);
    if (dest.kind == Operand::Kind::Memory) {
        idle(2);
        write(dest, Size::Byte, value | 0x80);
        prefetch();
        return;
    }
    write(dest, Size::Byte, value | 0x80);
    prefetch();
}

} // namespace ql
