#pragma once
// Small hand-assembled ROMs for tests of the QL machine: a reset vector and a
// few words of program.

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace qltest {

/// The top of 128K RAM: where the test ROMs put the stack.
constexpr uint32_t ROM_INITIAL_SSP = 0x00040000;
/// Where the test ROMs' programs start.
constexpr uint32_t ROM_START = 0x000100;

constexpr uint16_t STOP = 0x4E72;
constexpr uint16_t SUPERVISOR = 0x2700;

/// A ROM whose reset vector starts the given words at ROM_START.
inline std::vector<uint8_t> rom_running(std::initializer_list<uint16_t> program) {
    std::vector<uint8_t> rom(ROM_START + 2 * program.size(), 0);
    auto put = [&rom](uint32_t at, uint16_t word) {
        rom[at] = static_cast<uint8_t>(word >> 8);
        rom[at + 1] = static_cast<uint8_t>(word);
    };
    put(0, static_cast<uint16_t>(ROM_INITIAL_SSP >> 16));
    put(2, static_cast<uint16_t>(ROM_INITIAL_SSP));
    put(4, static_cast<uint16_t>(ROM_START >> 16));
    put(6, static_cast<uint16_t>(ROM_START));
    uint32_t at = ROM_START;
    for (uint16_t word : program) {
        put(at, word);
        at += 2;
    }
    return rom;
}

/// Where counting_rom() stores its count.
constexpr uint32_t COUNTER = 0x030000;

/// $100 MOVEQ #0,D0; $102 ADDQ.L #1,D0; $104 MOVE.L D0,($00030000).L;
/// $10A BRA.S $102 -- counting up for ever.
inline std::vector<uint8_t> counting_rom() {
    return rom_running({
        0x7000,                   // $100 MOVEQ #0,D0
        0x5280,                   // $102 ADDQ.L #1,D0
        0x23C0, 0x0003, 0x0000,   // $104 MOVE.L D0,($00030000).L
        0x60F6,                   // $10A BRA.S $102
    });
}

} // namespace qltest
