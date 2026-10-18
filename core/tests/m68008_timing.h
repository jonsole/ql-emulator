#pragma once
// What a SingleStepTests 68000 test means for a 68008.
//
// The suite records a 68000, whose data bus is 16 bits wide. A 68008 runs the
// same microcode over an 8-bit bus, so each word cycle becomes two byte
// cycles -- the even (high) byte, then the odd -- and costs 4 clocks more.
// Byte cycles and idle clocks are unchanged. That is the whole derivation
// docs/plan.md settles on; ticket #12 checks it against the 68008 tables in the
// M68000 User's Manual, which is the only way to catch it being wrong, since
// every other test shares it.

#include "m68000_file.h"

#include <cstdint>
#include <vector>

namespace ql::m68000_suite {

/// One byte cycle the 68008 should drive.
struct ByteCycle {
    Kind kind;                ///< never Idle
    uint64_t clock;           ///< clocks from the start of the instruction
    uint32_t function_code;
    uint32_t address;         ///< the byte's own address, A0 included
    uint8_t data;
};

struct Expected68008 {
    std::vector<ByteCycle> cycles;
    uint64_t length;          ///< 68008 clocks for the whole instruction
};

Expected68008 derive_68008(const std::vector<Transaction>& transactions);

} // namespace ql::m68000_suite
