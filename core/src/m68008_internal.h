#pragma once
// What the 68008 core's source files share and nothing else needs: the SR
// bits, the exception vectors and the sizes. The core is split by instruction
// group --
//
//   m68008.cpp             the bus, the prefetch queue, effective addresses,
//                          exception processing, and the dispatch
//   m68008_decode.cpp      which instruction each opcode is
//   m68008_move.cpp        MOVE, MOVEA, MOVEQ, LEA, PEA, MOVEP, MOVEM
//   m68008_system.cpp      the supervisor state: TRAP, CHK, RTE, STOP, the
//                          SR and USP moves...
//   m68008_flow.cpp        branches, jumps, calls, returns, LINK, UNLK
//   m68008_arithmetic.cpp  ADD, SUB, CMP, NEG, CLR, TST, EXT, SWAP, EXG...
//   m68008_logic.cpp       AND, OR, EOR, NOT, the bit instructions, Scc, TAS
//   m68008_shift.cpp       the shifts and rotates
//   m68008_bcd.cpp         ABCD, SBCD, NBCD
//   m68008_multiply.cpp    MULU, MULS, DIVU, DIVS
//
// -- and every routine is a member of M68008, declared in m68008.h.

#include "m68008.h"

#include <cstdint>

namespace ql {

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
constexpr uint16_t SR_MASK = 0x0700;   ///< the interrupt mask, I2-I0
constexpr uint16_t SR_CCR = 0x001F;
/// The bits of SR that exist on a 68000: T, S, the interrupt mask and the
/// condition codes. The rest read as zero whatever is written.
constexpr uint16_t SR_IMPLEMENTED = 0xA71F;

/// Exception vector numbers, from the M68000 User's Manual's vector table. The
/// vector's address is four times its number.
constexpr unsigned VECTOR_ADDRESS_ERROR = 3;
constexpr unsigned VECTOR_ILLEGAL = 4;
constexpr unsigned VECTOR_DIVIDE_BY_ZERO = 5;
constexpr unsigned VECTOR_CHK = 6;
constexpr unsigned VECTOR_TRAPV = 7;
constexpr unsigned VECTOR_PRIVILEGE = 8;
constexpr unsigned VECTOR_TRACE = 9;
constexpr unsigned VECTOR_LINE_A = 10;
constexpr unsigned VECTOR_LINE_F = 11;
/// Autovector n, for interrupt level n (1-7), is 24 + n.
constexpr unsigned VECTOR_AUTOVECTOR_0 = 24;
constexpr unsigned VECTOR_TRAP_0 = 32;

/// Thrown inside the core when a word or long access meets an odd address,
/// and caught by M68008::step, which abandons the instruction there and
/// stacks the group 0 frame. Never seen outside the core.
struct AddressError {
    uint32_t address;
    FunctionCode function_code;
    bool read;
};

/// The bits an operation of the given size works on.
inline uint32_t size_mask(Size size) {
    switch (size) {
    case Size::Byte: return 0xFF;
    case Size::Word: return 0xFFFF;
    case Size::Long: return 0xFFFFFFFF;
    }
    return 0;
}

} // namespace ql
