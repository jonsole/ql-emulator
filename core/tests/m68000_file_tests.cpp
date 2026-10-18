// The SingleStepTests .json.bin reader (m68000_file.h), against a file built
// here byte by byte from the layout in the suite's decode.py. Every value is a
// distinct literal, so a field read from the wrong offset shows up as the
// wrong number rather than passing by coincidence.

#include "m68000_file.h"
#include "test_main.h"

#include <stdexcept>

using namespace ql::m68000_suite;

namespace {

struct Writer {
    std::vector<uint8_t> bytes;

    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        u16(static_cast<uint16_t>(v));
        u16(static_cast<uint16_t>(v >> 16));
    }
    /// A block header: its length, which the reader ignores as decode.py
    /// does, then its magic number.
    void block(uint32_t magic) {
        u32(0);
        u32(magic);
    }
};

/// A state whose registers are base + their position in decode.py's order
/// (d0-d7, a0-a6, usp, ssp, sr, pc), so each register's value says where it
/// was read from.
void write_state(Writer& w, uint32_t base, uint32_t pf0, uint32_t pf1,
                 const std::vector<RamWord>& ram) {
    w.block(0x01234567);
    for (uint32_t i = 0; i < 19; i++) w.u32(base + i);
    w.u32(pf0);
    w.u32(pf1);
    w.u32(static_cast<uint32_t>(ram.size()));
    for (const RamWord& r : ram) {
        w.u32(r.address);
        w.u16(r.value);
    }
}

std::vector<uint8_t> one_case_file() {
    Writer w;
    w.u32(0x1A3F5D71);   // the file's magic
    w.u32(1);            // one test

    w.block(0xABC12367);
    w.block(0x89ABCDEF);
    const char name[] = "000 NOP 4e71";
    w.u32(sizeof name - 1);
    for (size_t i = 0; i < sizeof name - 1; i++) w.u8(static_cast<uint8_t>(name[i]));

    write_state(w, 0x100, 0x4E71, 0xD8B0, {{0x0DE9E8, 0x4E71}, {0x0DE9EA, 0xD8B0}});
    write_state(w, 0x200, 0xD8B0, 0x326B, {{0x0DE9EC, 0x326B}});

    w.block(0x456789AB);
    w.u32(6);            // the 68000's length
    w.u32(2);            // two transactions
    w.u8(2);             // a read...
    w.u32(4);            // ...of 4 clocks
    w.u32(6);            // FC
    w.u32(0x0DE9EC);     // address
    w.u32(0x326B);       // data
    w.u32(1);            // UDS
    w.u32(0);            // LDS
    w.u8(0);             // idle...
    w.u32(2);            // ...for 2 clocks, and nothing more
    return w.bytes;
}

} // namespace

TEST(reads_every_field_of_a_case) {
    std::vector<Case> cases = parse_test_file(one_case_file());
    CHECK_EQ(cases.size(), size_t{1});
    if (cases.size() != 1) return;
    const Case& c = cases[0];
    CHECK_EQ(c.name, std::string("000 NOP 4e71"));

    for (int i = 0; i < 8; i++) CHECK_EQ(c.initial.d[i], 0x100u + i);
    for (int i = 0; i < 7; i++) CHECK_EQ(c.initial.a[i], 0x108u + i);
    CHECK_EQ(c.initial.usp, 0x10Fu);
    CHECK_EQ(c.initial.ssp, 0x110u);
    CHECK_EQ(c.initial.sr, 0x111u);
    CHECK_EQ(c.initial.pc, 0x112u);
    CHECK_EQ(c.initial.prefetch[0], 0x4E71u);
    CHECK_EQ(c.initial.prefetch[1], 0xD8B0u);
    CHECK_EQ(c.initial.ram.size(), size_t{2});
    if (c.initial.ram.size() == 2) {
        CHECK_EQ(c.initial.ram[1].address, 0x0DE9EAu);
        CHECK_EQ(c.initial.ram[1].value, uint16_t{0xD8B0});
    }

    CHECK_EQ(c.final.d[0], 0x200u);
    CHECK_EQ(c.final.pc, 0x212u);
    CHECK_EQ(c.final.prefetch[1], 0x326Bu);
    CHECK_EQ(c.final.ram.size(), size_t{1});

    CHECK_EQ(c.length, 6u);
    CHECK_EQ(c.transactions.size(), size_t{2});
    if (c.transactions.size() != 2) return;
    const Transaction& r = c.transactions[0];
    CHECK(r.kind == Kind::Read);
    CHECK_EQ(r.cycles, 4u);
    CHECK_EQ(r.function_code, 6u);
    CHECK_EQ(r.address, 0x0DE9ECu);
    CHECK_EQ(r.data, 0x326Bu);
    CHECK(r.uds);
    CHECK(!r.lds);
    CHECK(c.transactions[1].kind == Kind::Idle);
    CHECK_EQ(c.transactions[1].cycles, 2u);
}

// A file cut short, or one that is not a test file at all, is an error to
// report, not a short list of tests to run and pass.
TEST(a_bad_file_is_refused) {
    std::vector<uint8_t> cut = one_case_file();
    cut.resize(cut.size() - 1);
    bool threw = false;
    try {
        parse_test_file(cut);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    std::vector<uint8_t> wrong = one_case_file();
    wrong[0] ^= 1;
    threw = false;
    try {
        parse_test_file(wrong);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

RUN_TESTS()
