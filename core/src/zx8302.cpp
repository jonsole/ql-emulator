#include "zx8302.h"

namespace ql {

void ZX8302::write_interrupt(uint8_t value) {
    // MAME's irq_acknowledge_w: the mask from bits 7-5, and the pending bits
    // set in 4-0 cleared. IPL1L follows from what is left pending.
    mask_ = value & 0xE0;
    pending_ = static_cast<uint8_t>(pending_ & ~(value & 0x1F));
}

uint8_t ZX8302::read_rtc(unsigned offset, uint64_t clock) const {
    // One second per 7,500,000 CPU clocks since it was set.
    constexpr uint64_t CLOCKS_PER_SECOND = 7'500'000;
    uint32_t seconds = rtc_seconds_ + static_cast<uint32_t>((clock - rtc_set_at_) / CLOCKS_PER_SECOND);
    return static_cast<uint8_t>(seconds >> (8 * (3 - (offset & 3))));
}

} // namespace ql
