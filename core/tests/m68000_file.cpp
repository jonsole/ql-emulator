#include "m68000_file.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace ql::m68000_suite {

namespace {

// The magic numbers decode.py asserts, one per kind of block.
constexpr uint32_t FILE_MAGIC = 0x1A3F5D71;
constexpr uint32_t CASE_MAGIC = 0xABC12367;
constexpr uint32_t NAME_MAGIC = 0x89ABCDEF;
constexpr uint32_t STATE_MAGIC = 0x01234567;
constexpr uint32_t TRANSACTIONS_MAGIC = 0x456789AB;

/// Little-endian reads that throw rather than run off the end.
struct Reader {
    const std::vector<uint8_t>& bytes;
    size_t at = 0;

    void need(size_t n) const {
        if (bytes.size() - at < n) {
            throw std::runtime_error("SingleStepTests file ends early, at offset " + std::to_string(at));
        }
    }
    uint8_t u8() {
        need(1);
        return bytes[at++];
    }
    uint16_t u16() {
        need(2);
        uint16_t v = static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8);
        at += 2;
        return v;
    }
    uint32_t u32() {
        need(4);
        uint32_t v = static_cast<uint32_t>(bytes[at]) | static_cast<uint32_t>(bytes[at + 1]) << 8
                     | static_cast<uint32_t>(bytes[at + 2]) << 16 | static_cast<uint32_t>(bytes[at + 3]) << 24;
        at += 4;
        return v;
    }
    void magic(uint32_t expected) {
        size_t where = at;
        uint32_t found = u32();
        if (found != expected) {
            throw std::runtime_error("SingleStepTests file has a wrong magic number at offset " + std::to_string(where));
        }
    }
    /// A block header: a length decode.py never uses, then the magic number.
    void block(uint32_t expected_magic) {
        u32();
        magic(expected_magic);
    }
};

State read_state(Reader& r) {
    r.block(STATE_MAGIC);
    State s{};
    for (uint32_t& d : s.d) d = r.u32();
    for (uint32_t& a : s.a) a = r.u32();
    s.usp = r.u32();
    s.ssp = r.u32();
    s.sr = r.u32();
    s.pc = r.u32();
    s.prefetch[0] = r.u32();
    s.prefetch[1] = r.u32();
    uint32_t words = r.u32();
    s.ram.reserve(words);
    for (uint32_t i = 0; i < words; i++) {
        RamWord w{};
        w.address = r.u32();
        w.value = r.u16();
        s.ram.push_back(w);
    }
    return s;
}

} // namespace

std::vector<Case> parse_test_file(const std::vector<uint8_t>& bytes) {
    Reader r{bytes};
    r.magic(FILE_MAGIC);
    uint32_t count = r.u32();
    std::vector<Case> cases;
    cases.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        Case c{};
        r.block(CASE_MAGIC);

        r.block(NAME_MAGIC);
        uint32_t length = r.u32();
        r.need(length);
        c.name.assign(reinterpret_cast<const char*>(bytes.data() + r.at), length);
        r.at += length;

        c.initial = read_state(r);
        c.final = read_state(r);

        r.block(TRANSACTIONS_MAGIC);
        c.length = r.u32();
        uint32_t transactions = r.u32();
        c.transactions.reserve(transactions);
        for (uint32_t j = 0; j < transactions; j++) {
            Transaction t{};
            uint8_t kind = r.u8();
            if (kind > static_cast<uint8_t>(Kind::WriteAddressError)) {
                throw std::runtime_error("SingleStepTests file has an unknown transaction type at offset "
                                         + std::to_string(r.at - 1));
            }
            t.kind = static_cast<Kind>(kind);
            t.cycles = r.u32();
            // An idle run is only its length; everything else is a full bus cycle.
            if (t.kind != Kind::Idle) {
                t.function_code = r.u32();
                t.address = r.u32();
                t.data = r.u32();
                t.uds = r.u32() != 0;
                t.lds = r.u32() != 0;
            }
            c.transactions.push_back(t);
        }
        cases.push_back(std::move(c));
    }
    return cases;
}

std::vector<Case> read_test_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_test_file(bytes);
}

} // namespace ql::m68000_suite
