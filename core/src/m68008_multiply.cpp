// The 68008 core: MULU, MULS, DIVU and DIVS, whose times depend on their
// operands.

#include "m68008_internal.h"

namespace ql {

namespace {

// The times of DIVU and DIVS, in 68000 clocks with the final prefetch
// included, follow the microcode's loop of one subtraction per quotient bit.
// Both follow Jorge Cwik's published analysis of the 68000's division
// microcode, and are checked here against every case in SingleStepTests.

unsigned divu_clocks(uint32_t dividend, uint16_t divisor) {
    // Overflow is seen before the loop starts.
    if ((dividend >> 16) >= divisor) return 10;
    unsigned cycles = 38;
    uint32_t shifted_divisor = static_cast<uint32_t>(divisor) << 16;
    for (int i = 0; i < 15; i++) {
        uint32_t before = dividend;
        dividend <<= 1;
        if (before & 0x80000000) {
            dividend -= shifted_divisor;
        } else {
            cycles += 2;
            if (dividend >= shifted_divisor) {
                dividend -= shifted_divisor;
                cycles--;
            }
        }
    }
    return cycles * 2;
}

unsigned divs_clocks(int32_t dividend, int16_t divisor) {
    unsigned cycles = 6;
    if (dividend < 0) cycles++;
    uint32_t magnitude = dividend < 0 ? 0u - static_cast<uint32_t>(dividend) : static_cast<uint32_t>(dividend);
    uint32_t divisor_magnitude = divisor < 0 ? 0u - static_cast<uint32_t>(divisor) : static_cast<uint32_t>(divisor);
    divisor_magnitude &= 0xFFFF;
    // An overflow of the magnitudes is seen before the loop.
    if ((magnitude >> 16) >= divisor_magnitude) return (cycles + 2) * 2;
    uint32_t quotient = magnitude / divisor_magnitude;
    cycles += 55;
    if (divisor >= 0) {
        if (dividend >= 0) {
            cycles--;
        } else {
            cycles++;
        }
    }
    // One more for each of the quotient's top 15 bits that is clear.
    for (int i = 0; i < 15; i++) {
        if (!(quotient & 0x8000)) cycles++;
        quotient <<= 1;
    }
    return cycles * 2;
}

} // namespace

void M68008::mul(uint16_t opcode) {
    // MULU and MULS <ea>,Dn: 1100 rrr0 11 / rrr1 11 mmm xxx. Word by word into
    // a long in Dn; N and Z from the long, V and C cleared, X untouched.
    bool is_signed = opcode & 0x0100;
    uint16_t source = static_cast<uint16_t>(read(resolve((opcode >> 3) & 7, opcode & 7, Size::Word), Size::Word));
    uint32_t& dn = regs_.d[(opcode >> 9) & 7];
    uint32_t result;
    unsigned n = 0;
    if (is_signed) {
        result = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(source))
                                       * static_cast<int32_t>(static_cast<int16_t>(dn)));
        // MULS: n is the number of 01 and 10 pairs in the source with a 0
        // appended below it.
        uint32_t bits = static_cast<uint32_t>(source) << 1;
        for (unsigned i = 0; i < 16; i++) {
            if (((bits >> i) & 3) == 1 || ((bits >> i) & 3) == 2) n++;
        }
    } else {
        result = static_cast<uint32_t>(source) * static_cast<uint16_t>(dn);
        // MULU: n is the number of ones in the source.
        for (unsigned i = 0; i < 16; i++) n += (source >> i) & 1;
    }
    dn = result;
    set_move_flags(result, Size::Long);
    // 38+2n clocks on the 68000 (User's Manual): the prefetch's 4, then
    // 34+2n inside (SingleStepTests).
    prefetch();
    idle(34 + 2 * n);
}

void M68008::div(uint16_t opcode) {
    // DIVU and DIVS <ea>,Dn: 1000 rrr0 11 / rrr1 11 mmm xxx. Dn's long by
    // the word, the quotient to the low word and the remainder (the
    // dividend's sign, for DIVS) to the high. The internal clocks all come
    // before the prefetch (SingleStepTests).
    bool is_signed = opcode & 0x0100;
    uint16_t divisor = static_cast<uint16_t>(read(resolve((opcode >> 3) & 7, opcode & 7, Size::Word), Size::Word));
    uint32_t& dn = regs_.d[(opcode >> 9) & 7];
    if (divisor == 0) {
        // Vector 5, returning to the next instruction, with Dn left alone:
        // 38(4/3) on the 68000 (User's Manual), so 8 internal clocks before
        // the frame. SingleStepTests has no case of it, so m68008_exception_
        // tests checks it; the manual leaves the condition codes undefined
        // and nothing here settles them, so they are left as they were.
        idle(8);
        exception(VECTOR_DIVIDE_BY_ZERO, regs_.pc - 2);
        return;
    }

    uint32_t quotient;
    uint32_t remainder;
    bool overflow;
    unsigned clocks;
    if (is_signed) {
        int32_t dividend = static_cast<int32_t>(dn);
        int32_t signed_divisor = static_cast<int16_t>(divisor);
        clocks = divs_clocks(dividend, static_cast<int16_t>(divisor));
        // INT32_MIN / -1 does not fit even in 32 bits; it is an overflow
        // like any quotient outside a word.
        int64_t q = static_cast<int64_t>(dividend) / signed_divisor;
        int64_t r = static_cast<int64_t>(dividend) % signed_divisor;
        overflow = q < -32768 || q > 32767;
        quotient = static_cast<uint32_t>(q);
        remainder = static_cast<uint32_t>(r);
    } else {
        clocks = divu_clocks(dn, divisor);
        quotient = dn / divisor;
        remainder = dn % divisor;
        overflow = quotient > 0xFFFF;
    }

    uint16_t ccr = regs_.sr & SR_X;
    if (overflow) {
        // Dn is left alone. V is set and C cleared; the manual leaves N and Z
        // undefined, and the 68000 sets N and clears Z (SingleStepTests).
        ccr |= SR_V | SR_N;
    } else {
        dn = (remainder & 0xFFFF) << 16 | (quotient & 0xFFFF);
        if (quotient & 0x8000) ccr |= SR_N;
        if ((quotient & 0xFFFF) == 0) ccr |= SR_Z;
    }
    regs_.sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | ccr);
    idle(clocks - 4);
    prefetch();
}

} // namespace ql
