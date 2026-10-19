#pragma once
// Decoding for the 68008 core: which instruction each of the 65,536 opcodes
// is, and whether it is one at all. Internal to the core; m68008.cpp dispatches
// on it.

#include <cstdint>

namespace ql {

/// What an opcode is, as far as the core's dispatch goes: one value per
/// routine in M68008.
enum class Instruction : uint8_t {
    /// Not a 68000 instruction: the illegal-instruction exception, vector 4.
    Illegal,
    /// Line A (1010) and line F (1111), each with its own vector.
    LineA,
    LineF,
    /// A 68000 instruction that a later Stage 1 ticket implements.
    NotYet,

    Nop,
    Moveq,
    Move,
    Movea,
    Lea,
    Pea,
    Trap,
    Trapv,
    Chk,
    Rte,
    Stop,
    Reset,
    MoveUsp,
    LogicToCcr,     ///< ORI, ANDI and EORI to CCR
    LogicToSr,      ///< ORI, ANDI and EORI to SR
    MoveToCcr,
    MoveToSr,
    MoveFromSr,
};

/// The instruction an opcode encodes, from a table built on first use.
Instruction decode(uint16_t opcode);

} // namespace ql
