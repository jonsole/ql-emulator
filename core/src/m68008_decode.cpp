#include "m68008_decode.h"

#include <array>

namespace ql {

namespace {

// The effective address field is a mode (bits 5-3) and a register (bits 2-0);
// mode 7 uses the register field to pick (xxx).W 0, (xxx).L 1, (d16,PC) 2,
// (d8,PC,Xn) 3 and #imm 4, and 5-7 are nothing. Each instruction allows one of
// the categories below (User's Manual, the effective address encoding summary
// and each instruction's page).

bool is_mode(unsigned mode, unsigned reg) {
    return mode != 7 || reg <= 4;
}

/// Every mode but An.
bool is_data(unsigned mode, unsigned reg) {
    return mode != 1 && is_mode(mode, reg);
}

/// The modes that name a place without reading or writing it: (An),
/// (d16,An), (d8,An,Xn), (xxx).W, (xxx).L, (d16,PC), (d8,PC,Xn).
bool is_control(unsigned mode, unsigned reg) {
    return mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3);
}

/// The modes that can be written: no PC-relative, no immediate.
bool is_alterable(unsigned mode, unsigned reg) {
    return mode != 7 || reg <= 1;
}

bool is_data_alterable(unsigned mode, unsigned reg) {
    return mode != 1 && is_alterable(mode, reg);
}

bool is_memory_alterable(unsigned mode, unsigned reg) {
    return mode >= 2 && is_alterable(mode, reg);
}

/// A size field in bits 7-6 of 00, 01 or 10 (byte, word, long). 11 is no size,
/// and usually a different instruction.
bool has_size(uint16_t opcode) {
    return (opcode & 0x00C0) != 0x00C0;
}

bool is_byte(uint16_t opcode) {
    return (opcode & 0x00C0) == 0;
}

Instruction is_if(bool valid, Instruction instruction) {
    return valid ? instruction : Instruction::Illegal;
}

/// 0000: the immediate instructions, the bit instructions and MOVEP.
Instruction line_0(uint16_t opcode, unsigned mode, unsigned reg) {
    switch (opcode) {
    case 0x003C:
    case 0x023C:
    case 0x0A3C:
        return Instruction::LogicToCcr;
    case 0x007C:
    case 0x027C:
    case 0x0A7C:
        return Instruction::LogicToSr;
    }
    if (opcode & 0x0100) {
        // MOVEP (0000 ddd1 oo00 1aaa), or BTST, BCHG, BCLR and BSET with the
        // bit number in Dn. BTST can test an immediate; the others write.
        if (mode == 1) return Instruction::Movep;
        if (is_byte(opcode)) return is_if(is_data(mode, reg), Instruction::Bit);
        return is_if(is_data_alterable(mode, reg), Instruction::Bit);
    }
    switch ((opcode >> 9) & 7) {
    case 4:
        // BTST, BCHG, BCLR and BSET with the bit number in an extension word.
        if (is_byte(opcode)) return is_if(is_data(mode, reg) && !(mode == 7 && reg == 4), Instruction::Bit);
        return is_if(is_data_alterable(mode, reg), Instruction::Bit);
    case 7:
        // MOVES, from the 68010 on.
        return Instruction::Illegal;
    case 2:
    case 3:
        // SUBI and ADDI.
        return is_if(has_size(opcode) && is_data_alterable(mode, reg), Instruction::AddiSubi);
    case 6:
        // CMPI, to a data alterable operand only: its PC-relative modes are
        // the 68020's.
        return is_if(has_size(opcode) && is_data_alterable(mode, reg), Instruction::Cmpi);
    default:
        // ORI, ANDI and EORI (0, 1 and 5), to a data alterable operand; to
        // CCR and SR they were taken above.
        return is_if(has_size(opcode) && is_data_alterable(mode, reg), Instruction::LogicImmediate);
    }
}

/// 0001, 0010, 0011: MOVE and MOVEA, sized byte, long and word.
Instruction line_move(uint16_t opcode, unsigned mode, unsigned reg) {
    bool byte = (opcode & 0x3000) == 0x1000;
    unsigned dest_mode = (opcode >> 6) & 7;
    unsigned dest_reg = (opcode >> 9) & 7;
    if (!is_mode(mode, reg) || (byte && mode == 1)) return Instruction::Illegal;
    if (dest_mode == 1) return is_if(!byte, Instruction::Movea);
    return is_if(is_data_alterable(dest_mode, dest_reg), Instruction::Move);
}

/// 0100 1110: TRAP, LINK, UNLK, MOVE USP, the 4E7x group, JSR and JMP.
Instruction line_4e(uint16_t opcode, unsigned mode, unsigned reg) {
    switch (opcode & 0x00C0) {
    case 0x0040:
        switch (mode) {
        case 0:
        case 1:
            return Instruction::Trap;
        case 2:
            return Instruction::Link;
        case 3:
            return Instruction::Unlk;
        case 4:
        case 5:
            return Instruction::MoveUsp;
        case 6:
            switch (opcode) {
            case 0x4E70: return Instruction::Reset;
            case 0x4E71: return Instruction::Nop;
            case 0x4E72: return Instruction::Stop;
            case 0x4E73: return Instruction::Rte;
            case 0x4E75: return Instruction::Rts;
            case 0x4E76: return Instruction::Trapv;
            case 0x4E77: return Instruction::Rtr;
            }
            return Instruction::Illegal;   // 4E74 is the 68010's RTD
        }
        return Instruction::Illegal;   // 4E78-4E7F: the 68010's MOVEC
    case 0x0080:
        return is_if(is_control(mode, reg), Instruction::Jsr);
    case 0x00C0:
        return is_if(is_control(mode, reg), Instruction::Jmp);
    }
    return Instruction::Illegal;
}

/// 0100: everything else that fits nowhere tidier.
Instruction line_4(uint16_t opcode, unsigned mode, unsigned reg) {
    if (opcode & 0x0100) {
        // CHK (0100 ddd1 10) and LEA (0100 ddd1 11). 00 is the 68020's
        // CHK.L.
        switch (opcode & 0x01C0) {
        case 0x0180: return is_if(is_data(mode, reg), Instruction::Chk);
        case 0x01C0: return is_if(is_control(mode, reg), Instruction::Lea);
        }
        return Instruction::Illegal;
    }
    switch ((opcode >> 8) & 0xF) {
    case 0x0:
        // NEGX; with size 11, MOVE from SR.
        if (!has_size(opcode)) return is_if(is_data_alterable(mode, reg), Instruction::MoveFromSr);
        return is_if(is_data_alterable(mode, reg), Instruction::NegNegxClr);
    case 0x2:
        // CLR; size 11 is the 68010's MOVE from CCR.
        return is_if(has_size(opcode) && is_data_alterable(mode, reg), Instruction::NegNegxClr);
    case 0x4:
        // NEG; with size 11, MOVE to CCR.
        if (!has_size(opcode)) return is_if(is_data(mode, reg), Instruction::MoveToCcr);
        return is_if(is_data_alterable(mode, reg), Instruction::NegNegxClr);
    case 0x6:
        // NOT; with size 11, MOVE to SR.
        if (!has_size(opcode)) return is_if(is_data(mode, reg), Instruction::MoveToSr);
        return is_if(is_data_alterable(mode, reg), Instruction::Not);
    case 0x8:
        switch (opcode & 0x00C0) {
        case 0x0000:
            return is_if(is_data_alterable(mode, reg), Instruction::Nbcd);
        case 0x0040:
            if (mode == 0) return Instruction::Swap;
            return is_if(is_control(mode, reg), Instruction::Pea);
        }
        // EXT with Dn; otherwise MOVEM, registers to memory.
        if (mode == 0) return Instruction::Ext;
        return is_if((is_control(mode, reg) && is_alterable(mode, reg)) || mode == 4, Instruction::Movem);
    case 0xA:
        // TST, and with size 11 TAS -- apart from ILLEGAL itself, 4AFC, which
        // TAS's encoding would make TAS #imm.
        if (!has_size(opcode)) return is_if(is_data_alterable(mode, reg), Instruction::Tas);
        return is_if(is_data_alterable(mode, reg), Instruction::Tst);
    case 0xC:
        // MOVEM, memory to registers. 4C00-4C7F are the 68020's MULL and DIVL.
        if (!(opcode & 0x0080)) return Instruction::Illegal;
        return is_if(is_control(mode, reg) || mode == 3, Instruction::Movem);
    case 0xE:
        return line_4e(opcode, mode, reg);
    }
    return Instruction::Illegal;
}

/// 0101: ADDQ, SUBQ, Scc and DBcc.
Instruction line_5(uint16_t opcode, unsigned mode, unsigned reg) {
    if (!has_size(opcode)) {
        if (mode == 1) return Instruction::Dbcc;
        // Scc; the rest are the 68020's TRAPcc.
        return is_if(is_data_alterable(mode, reg), Instruction::Scc);
    }
    // ADDQ and SUBQ.
    if (mode == 1 && is_byte(opcode)) return Instruction::Illegal;
    return is_if(is_alterable(mode, reg), Instruction::AddqSubq);
}

/// 1000 and 1100: OR, DIVU, DIVS and SBCD; AND, MULU, MULS, ABCD and EXG.
Instruction line_8_c(uint16_t opcode, unsigned mode, unsigned reg) {
    bool and_line = (opcode >> 12) == 0xC;
    switch (opcode & 0x01C0) {
    case 0x00C0:
    case 0x01C0:
        // MULU and MULS on line C, DIVU and DIVS on line 8.
        return is_if(is_data(mode, reg), and_line ? Instruction::Mul : Instruction::Div);
    }
    if (!(opcode & 0x0100)) return is_if(is_data(mode, reg), Instruction::AndOr);   // to Dn
    if (mode <= 1) {
        if (is_byte(opcode)) return Instruction::AbcdSbcd;
        // EXG Dx,Dy (opmode 01000), Ax,Ay (01001) and Dx,Ay (10001). On line
        // 8 these are the 68020's PACK and UNPK.
        unsigned exg = opcode & 0x01F8;
        return is_if(and_line && (exg == 0x0140 || exg == 0x0148 || exg == 0x0188), Instruction::Exg);
    }
    return is_if(is_memory_alterable(mode, reg), Instruction::AndOr);   // from Dn to memory
}

/// 1001 and 1101: SUB, SUBA and SUBX; ADD, ADDA and ADDX.
Instruction line_9_d(uint16_t opcode, unsigned mode, unsigned reg) {
    if (!has_size(opcode)) return is_if(is_mode(mode, reg), Instruction::AddaSuba);
    if (opcode & 0x0100) {
        if (mode <= 1) return Instruction::AddxSubx;
        return is_if(is_memory_alterable(mode, reg), Instruction::AddSub);
    }
    if (mode == 1 && is_byte(opcode)) return Instruction::Illegal;
    return is_if(is_mode(mode, reg), Instruction::AddSub);
}

/// 1011: CMP, CMPA, CMPM and EOR.
Instruction line_b(uint16_t opcode, unsigned mode, unsigned reg) {
    if (!has_size(opcode)) return is_if(is_mode(mode, reg), Instruction::Cmpa);
    if (opcode & 0x0100) {
        if (mode == 1) return Instruction::Cmpm;
        return is_if(is_data_alterable(mode, reg), Instruction::Eor);
    }
    if (mode == 1 && is_byte(opcode)) return Instruction::Illegal;
    return is_if(is_mode(mode, reg), Instruction::Cmp);
}

/// 1110: the shifts and rotates.
Instruction line_e(uint16_t opcode, unsigned mode, unsigned reg) {
    // The type -- 00 AS, 01 LS, 10 ROX, 11 RO -- is in bits 4-3 on a data
    // register and bits 10-9 on memory.
    if (has_size(opcode)) return Instruction::Shift;
    // On memory, one bit, word-sized. Bit 11 set is the 68020's bit fields.
    if (opcode & 0x0800) return Instruction::Illegal;
    return is_if(is_memory_alterable(mode, reg), Instruction::Shift);
}

Instruction classify(uint16_t opcode) {
    unsigned mode = (opcode >> 3) & 7;
    unsigned reg = opcode & 7;
    switch (opcode >> 12) {
    case 0x0: return line_0(opcode, mode, reg);
    case 0x1:
    case 0x2:
    case 0x3: return line_move(opcode, mode, reg);
    case 0x4: return line_4(opcode, mode, reg);
    case 0x5: return line_5(opcode, mode, reg);
    case 0x6:
        // Bcc and BRA (condition 0), and BSR (condition 1): every opcode is one.
        if (((opcode >> 8) & 0xF) == 1) return Instruction::Bsr;
        return Instruction::Bcc;
    case 0x7: return is_if(!(opcode & 0x0100), Instruction::Moveq);
    case 0x8:
    case 0xC: return line_8_c(opcode, mode, reg);
    case 0x9:
    case 0xD: return line_9_d(opcode, mode, reg);
    case 0xA: return Instruction::LineA;
    case 0xB: return line_b(opcode, mode, reg);
    case 0xE: return line_e(opcode, mode, reg);
    case 0xF: return Instruction::LineF;
    }
    return Instruction::Illegal;
}

} // namespace

Instruction decode(uint16_t opcode) {
    // Built once, on first use: 64 KB, one entry per opcode.
    static const std::array<Instruction, 0x10000> table = [] {
        std::array<Instruction, 0x10000> t{};
        for (uint32_t opcode = 0; opcode < 0x10000; opcode++) {
            t[opcode] = classify(static_cast<uint16_t>(opcode));
        }
        return t;
    }();
    return table[opcode];
}

} // namespace ql
