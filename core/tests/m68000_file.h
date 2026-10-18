#pragma once
// SingleStepTests/m68000, read straight from its .json.bin files.
//
// The suite ships each instruction's tests in a small binary form and a
// decode.py that turns it into JSON. Reading the binary here instead keeps a
// JSON library and a decode step out of the build. The layout is decode.py's,
// at the commit scripts/fetch-m68000-tests.ps1 pins: little-endian throughout,
// each block opening with its length and a magic number.

#include <cstdint>
#include <string>
#include <vector>

namespace ql::m68000_suite {

/// A transaction's type, numbered as the file numbers them.
enum class Kind : uint8_t {
    Idle = 0,
    Write = 1,
    Read = 2,
    Tas = 3,                  ///< TAS's indivisible read-modify-write
    ReadAddressError = 4,     ///< a read the 68000 abandoned: AS never asserted
    WriteAddressError = 5,    ///< likewise, a write
};

/// One 68000 bus cycle, or a run of idle clocks. The address is always even:
/// the 68000 has no A0, and says which byte of the word it wants with UDS
/// (D8-D15, the even byte) and LDS (D0-D7, the odd one). `data` is the whole
/// data bus, so a byte read through UDS alone of 0xAB appears as 0xAB00.
struct Transaction {
    Kind kind;
    uint32_t cycles;
    uint32_t function_code;   ///< unused for Idle
    uint32_t address;
    uint32_t data;
    bool uds;
    bool lds;
};

/// A word of memory. The 68000 is a 16-bit part, so the file gives RAM in
/// words at even addresses.
struct RamWord {
    uint32_t address;
    uint16_t value;
};

struct State {
    uint32_t d[8];
    uint32_t a[7];
    uint32_t usp;
    uint32_t ssp;
    uint32_t sr;
    uint32_t pc;              ///< the next prefetch address: see Registers::pc
    uint32_t prefetch[2];     ///< IR, then IRC
    std::vector<RamWord> ram;
};

struct Case {
    std::string name;
    State initial;
    State final;
    std::vector<Transaction> transactions;
    uint32_t length;          ///< 68000 clocks; always the sum of the transactions'
};

/// Parses a whole file. Throws std::runtime_error, naming the offset, on a
/// wrong magic number or a file that ends early.
std::vector<Case> parse_test_file(const std::vector<uint8_t>& bytes);

/// Reads and parses the file at `path`. Throws std::runtime_error if it cannot
/// be read.
std::vector<Case> read_test_file(const std::string& path);

} // namespace ql::m68000_suite
