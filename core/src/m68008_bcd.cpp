// The 68008 core: packed decimal -- ABCD, SBCD and NBCD.

#include "m68008_internal.h"

namespace ql {

uint8_t M68008::decimal(uint8_t dst, uint8_t src, bool subtract) {
    // The binary sum or difference with X, then a correction of 6 in each
    // digit that carried, borrowed, or (adding) came out above 9. C and X
    // are the decimal carry or borrow. The manual leaves N and V undefined:
    // the 68000 sets N from the result's top bit and V when the correction
    // carries the top bit from clear to set (adding) or set to clear
    // (subtracting). This is the formulation Flamewing verified against real
    // 68000s (68k-bcd-verifier), and what SingleStepTests records.
    uint32_t x = (regs_.sr & SR_X) ? 1 : 0;
    uint32_t a = dst;
    uint32_t b = src;
    uint32_t result;
    bool carry;
    bool overflow;
    if (!subtract) {
        uint32_t sum = (a + b + x) & 0xFF;
        // Bit 3 and bit 7: a binary carry out of each digit.
        uint32_t binary_carry = ((a & b) | (~sum & a) | (~sum & b)) & 0x88;
        // A digit above 9 carries in decimal.
        uint32_t decimal_carry = (((sum + 0x66) ^ sum) & 0x110) >> 1;
        uint32_t carries = binary_carry | decimal_carry;
        uint32_t correction = carries - (carries >> 2);   // 6 for each digit
        result = (sum + correction) & 0xFF;
        carry = ((binary_carry | (sum & ~result)) & 0x80) != 0;
        overflow = ((~sum & result) & 0x80) != 0;
    } else {
        uint32_t difference = (a - b - x) & 0xFF;
        uint32_t borrows = ((~a & b) | (difference & ~a) | (difference & b)) & 0x88;
        uint32_t correction = borrows - (borrows >> 2);
        result = (difference - correction) & 0xFF;
        carry = ((borrows | (~difference & result)) & 0x80) != 0;
        overflow = ((difference & ~result) & 0x80) != 0;
    }

    // Z is only ever cleared, as for ADDX, so that a multi-byte result is
    // zero only if every byte is.
    uint16_t ccr = regs_.sr & SR_Z;
    if (result != 0) ccr = 0;
    if (carry) ccr |= SR_X | SR_C;
    if (result & 0x80) ccr |= SR_N;
    if (overflow) ccr |= SR_V;
    regs_.sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | ccr);
    return static_cast<uint8_t>(result);
}

void M68008::abcd_sbcd(uint16_t opcode) {
    // ABCD and SBCD: 1100 / 1000 xxx1 0000 myyy, Dy to Dx with m clear, or
    // -(Ay) to -(Ax) with it set. On registers, 2 internal clocks after the
    // prefetch; in memory, ADDX's pattern for a byte (SingleStepTests).
    bool subtract = (opcode >> 12) == 0x8;
    unsigned rx = (opcode >> 9) & 7;
    unsigned ry = opcode & 7;
    if (!(opcode & 0x0008)) {
        uint8_t result = decimal(static_cast<uint8_t>(regs_.d[rx]), static_cast<uint8_t>(regs_.d[ry]), subtract);
        regs_.d[rx] = (regs_.d[rx] & 0xFFFFFF00) | result;
        prefetch();
        idle(2);
        return;
    }
    idle(2);
    uint32_t address;
    uint8_t source = static_cast<uint8_t>(read_predecrement_low_first(ry, Size::Byte, address));
    uint8_t destination = static_cast<uint8_t>(read_predecrement_low_first(rx, Size::Byte, address));
    uint8_t result = decimal(destination, source, subtract);
    prefetch();
    write_byte(address, result, data_space());
}

void M68008::nbcd(uint16_t opcode) {
    // NBCD <ea>: 0100 1000 00 mmm xxx, 0 - operand - X in decimal. On Dn, 2
    // internal clocks after the prefetch; in memory, a read-modify-write.
    Operand dest = resolve((opcode >> 3) & 7, opcode & 7, Size::Byte);
    uint8_t result = decimal(0, static_cast<uint8_t>(read(dest, Size::Byte)), true);
    prefetch();
    write(dest, Size::Byte, result);
    if (dest.kind == Operand::Kind::DataRegister) idle(2);
}

} // namespace ql
