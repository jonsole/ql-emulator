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

FunctionCode M68008::program_space() const {
    return (regs_.sr & SR_S) ? FunctionCode::SupervisorProgram : FunctionCode::UserProgram;
}

void M68008::prefetch() {
    regs_.ir = regs_.irc;
    regs_.irc = read_word(regs_.pc, program_space());
    regs_.pc += 2;
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
    // Every opcode will decode by the end of Stage 1 -- the ones the 68000
    // does not define take the illegal-instruction, line A or line F
    // exceptions (ticket #3). Until then, an opcode with no implementation is
    // a gap in the core, and saying so beats running on in the wrong state.
    throw std::logic_error("M68008::step: opcode not implemented yet");
}

} // namespace ql
