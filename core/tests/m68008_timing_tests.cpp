// The derivation of a 68008's bus cycles from a 68000 test (m68008_timing.h).
// Every expected value here is worked out by hand from the rule -- a word cycle
// is two byte cycles, even byte first, 4 clocks each -- not taken from the
// code's output.

#include "m68008_timing.h"
#include "test_main.h"

using namespace ql::m68000_suite;

namespace {

Transaction word(Kind kind, uint32_t address, uint32_t data) {
    return {kind, 4, 6, address, data, true, true};
}

} // namespace

// A NOP's whole bus activity: one word fetched. On the 68000 that is one
// 4-clock cycle; on the 68008, the even byte at clock 0, the odd at clock 4,
// and 8 clocks in all.
TEST(word_read_becomes_two_byte_reads) {
    Expected68008 e = derive_68008({word(Kind::Read, 0x001000, 0x4E71)});
    CHECK_EQ(e.cycles.size(), size_t{2});
    if (e.cycles.size() != 2) return;
    CHECK(e.cycles[0].kind == Kind::Read);
    CHECK_EQ(e.cycles[0].address, 0x001000u);
    CHECK_EQ(e.cycles[0].data, uint8_t{0x4E});
    CHECK_EQ(e.cycles[0].clock, uint64_t{0});
    CHECK_EQ(e.cycles[0].function_code, 6u);
    CHECK(e.cycles[1].kind == Kind::Read);
    CHECK_EQ(e.cycles[1].address, 0x001001u);
    CHECK_EQ(e.cycles[1].data, uint8_t{0x71});
    CHECK_EQ(e.cycles[1].clock, uint64_t{4});
    CHECK_EQ(e.length, uint64_t{8});
}

// A long written low word first, as a predecrement does it, after 2 idle
// clocks. Idle clocks put nothing on the bus but push everything after them
// back; each word then splits as above. Bytes at 2, 6, 10 and 14; 2 + 8 + 8 =
// 18 clocks.
TEST(idle_then_long_write) {
    Expected68008 e = derive_68008({
        {Kind::Idle, 2, 0, 0, 0, false, false},
        word(Kind::Write, 0x002002, 0x5678),
        word(Kind::Write, 0x002000, 0x1234),
    });
    CHECK_EQ(e.cycles.size(), size_t{4});
    if (e.cycles.size() != 4) return;
    const uint32_t addresses[4] = {0x002002, 0x002003, 0x002000, 0x002001};
    const uint8_t data[4] = {0x56, 0x78, 0x12, 0x34};
    const uint64_t clocks[4] = {2, 6, 10, 14};
    for (int i = 0; i < 4; i++) {
        CHECK(e.cycles[i].kind == Kind::Write);
        CHECK_EQ(e.cycles[i].address, addresses[i]);
        CHECK_EQ(e.cycles[i].data, data[i]);
        CHECK_EQ(e.cycles[i].clock, clocks[i]);
    }
    CHECK_EQ(e.length, uint64_t{18});
}

// Byte cycles cost the same on both parts. The file gives the even address
// either way: UDS alone means the even byte, on D8-D15; LDS alone the odd
// byte at address + 1, on D0-D7. So 0xAB read at 0x3000 at clock 0, then 0xCD
// written at 0x3001 at clock 4, and 8 clocks in all.
TEST(byte_cycles_are_unchanged) {
    Expected68008 e = derive_68008({
        {Kind::Read, 4, 5, 0x003000, 0xAB00, true, false},
        {Kind::Write, 4, 5, 0x003000, 0x00CD, false, true},
    });
    CHECK_EQ(e.cycles.size(), size_t{2});
    if (e.cycles.size() != 2) return;
    CHECK(e.cycles[0].kind == Kind::Read);
    CHECK_EQ(e.cycles[0].address, 0x003000u);
    CHECK_EQ(e.cycles[0].data, uint8_t{0xAB});
    CHECK_EQ(e.cycles[0].clock, uint64_t{0});
    CHECK(e.cycles[1].kind == Kind::Write);
    CHECK_EQ(e.cycles[1].address, 0x003001u);
    CHECK_EQ(e.cycles[1].data, uint8_t{0xCD});
    CHECK_EQ(e.cycles[1].clock, uint64_t{4});
    CHECK_EQ(e.length, uint64_t{8});
}

// An access that takes an address error never reaches the bus: the 68000
// sees the odd address before it starts the cycle, and the suite records the
// attempt as one transaction of its own, 4 clocks long. On the 68008 the
// check comes at the same point, before any byte cycle, so it is 4 clocks
// with nothing on the bus. A word read at 0, the attempt from clock 8, 8
// internal clocks, a word written from clock 20: bytes at 0, 4, 20 and 24,
// 28 in all.
TEST(an_address_error_is_four_clocks_with_nothing_on_the_bus) {
    Expected68008 e = derive_68008({
        word(Kind::Read, 0x001000, 0x4E71),
        {Kind::ReadAddressError, 4, 5, 0x002000, 0, true, true},
        {Kind::Idle, 8, 0, 0, 0, false, false},
        word(Kind::Write, 0x003000, 0x1234),
    });
    CHECK_EQ(e.cycles.size(), size_t{4});
    if (e.cycles.size() != 4) return;
    const Kind kinds[4] = {Kind::Read, Kind::Read, Kind::Write, Kind::Write};
    const uint64_t clocks[4] = {0, 4, 20, 24};
    for (int i = 0; i < 4; i++) {
        CHECK(e.cycles[i].kind == kinds[i]);
        CHECK_EQ(e.cycles[i].clock, clocks[i]);
    }
    CHECK_EQ(e.length, uint64_t{28});
}

RUN_TESTS()
