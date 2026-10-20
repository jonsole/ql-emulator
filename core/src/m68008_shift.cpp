// The 68008 core: the shifts and rotates -- ASL, ASR, LSL, LSR, ROL, ROR,
// ROXL and ROXR, on a data register by a count, or on a word in memory by
// one bit.

#include "m68008_internal.h"

namespace ql {

uint32_t M68008::shift_value(unsigned type, bool left, uint32_t value, unsigned count, Size size) {
    // One bit at a time, as the definitions read: the count is at most 63,
    // and a loop is the plainest way to get C, X and ASL's V right at every
    // count, past the operand's size included.
    uint32_t mask = size_mask(size);
    uint32_t sign = (mask >> 1) + 1;
    value &= mask;
    bool x = regs_.sr & SR_X;
    bool carry = false;
    bool overflow = false;
    for (unsigned i = 0; i < count; i++) {
        bool out;
        if (left) {
            out = (value & sign) != 0;
            value = (value << 1) & mask;
            if (type == 2 && x) value |= 1;     // ROXL: X comes in
            if (type == 3 && out) value |= 1;   // ROL: the bit comes round
            // ASL sets V if the sign bit changes at any point.
            if (type == 0 && ((value & sign) != 0) != out) overflow = true;
        } else {
            out = (value & 1) != 0;
            uint32_t in = 0;
            if (type == 0) in = value & sign;   // ASR: the sign is kept
            if (type == 2 && x) in = sign;      // ROXR: X comes in
            if (type == 3 && out) in = sign;    // ROR: the bit comes round
            value = (value >> 1) | in;
        }
        carry = out;
        // The rotates without X leave it alone.
        if (type != 3) x = out;
    }
    // A count of zero clears C, but ROXL and ROXR copy X into it. X is left
    // alone either way.
    if (count == 0 && type == 2) carry = x;

    uint16_t ccr = x ? SR_X : 0;
    if (value & sign) ccr |= SR_N;
    if (value == 0) ccr |= SR_Z;
    if (overflow) ccr |= SR_V;
    if (carry) ccr |= SR_C;
    regs_.sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | ccr);
    return value;
}

void M68008::shift(uint16_t opcode) {
    bool left = opcode & 0x0100;
    if ((opcode & 0x00C0) == 0x00C0) {
        // On memory: 1110 0tt d 11 mmm xxx, a word shifted by one bit, read,
        // prefetched past and written back.
        Operand dest = resolve((opcode >> 3) & 7, opcode & 7, Size::Word);
        uint32_t result = shift_value((opcode >> 9) & 3, left, read(dest, Size::Word), 1, Size::Word);
        finish_read_modify_write(dest, Size::Word, result, 0);
        return;
    }
    // On Dn: 1110 ccc d ss i tt rrr. With i clear the count is ccc, 0
    // meaning 8; with it set, Dc modulo 64.
    Size size = size_field(opcode);
    unsigned count = (opcode >> 9) & 7;
    if (opcode & 0x0020) {
        count = regs_.d[count] & 63;
    } else if (count == 0) {
        count = 8;
    }
    uint32_t& dn = regs_.d[opcode & 7];
    uint32_t mask = size_mask(size);
    uint32_t result = shift_value((opcode >> 3) & 3, left, dn, count, size);
    dn = (dn & ~mask) | result;
    // 2 internal clocks a bit, after 2 more for a byte or word and 4 for a
    // long (SingleStepTests; the User's Manual's 6+2n and 8+2n).
    prefetch();
    idle((size == Size::Long ? 4 : 2) + 2 * count);
}

} // namespace ql
