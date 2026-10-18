#include "m68008_timing.h"

namespace ql::m68000_suite {

/// A 68008 byte cycle without wait states, which is all the suite has.
constexpr uint32_t BYTE_CYCLE_CLOCKS = 4;

Expected68008 derive_68008(const std::vector<Transaction>& transactions) {
    Expected68008 expected{};
    uint64_t clock = 0;
    for (const Transaction& t : transactions) {
        if (t.kind == Kind::Idle) {
            clock += t.cycles;
            continue;
        }
        // The even byte rides D8-D15, so it is the high half of the data bus.
        uint8_t even = static_cast<uint8_t>(t.data >> 8);
        uint8_t odd = static_cast<uint8_t>(t.data);
        if (t.uds != t.lds) {
            // A byte cycle: the same on both parts.
            if (t.uds) {
                expected.cycles.push_back({t.kind, clock, t.function_code, t.address, even});
            } else {
                expected.cycles.push_back({t.kind, clock, t.function_code, t.address + 1, odd});
            }
            clock += t.cycles;
            continue;
        }
        expected.cycles.push_back({t.kind, clock, t.function_code, t.address, even});
        expected.cycles.push_back({t.kind, clock + BYTE_CYCLE_CLOCKS, t.function_code, t.address + 1, odd});
        // The second byte cycle takes the word cycle's own length, so anything
        // the 68000 spent beyond the minimum stays in the count.
        clock += BYTE_CYCLE_CLOCKS + t.cycles;
    }
    expected.length = clock;
    return expected;
}

} // namespace ql::m68000_suite
