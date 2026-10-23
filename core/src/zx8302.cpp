#include "zx8302.h"

namespace ql {

void ZX8302::write_interrupt(uint8_t value) {
    // MAME's irq_acknowledge_w: the mask from bits 7-5, and the pending bits
    // set in 4-0 cleared. IPL1L follows from what is left pending.
    mask_ = value & 0xE0;
    pending_ = static_cast<uint8_t>(pending_ & ~(value & 0x1F));
    // The gap interrupt comes back while the gap is present and its mask is
    // on: "The gap interrupt regenerates as long as the gap level is present
    // and the mask is enabled. Minerva relies on this." With no microdrives
    // the gap is always present, so the JS ROM's microdrive driver, which
    // enables it to look for a cartridge, finds none and gives up: it reads
    // no sector header ($5448 times out) and the open fails, "not found".
    if (mask_ & MASK_GAP) pending_ |= INT_GAP;
}

uint8_t ZX8302::read_rtc(unsigned offset, uint64_t clock) const {
    // One second per 7,500,000 CPU clocks since it was set.
    constexpr uint64_t CLOCKS_PER_SECOND = 7'500'000;
    uint32_t seconds = rtc_seconds_ + static_cast<uint32_t>((clock - rtc_set_at_) / CLOCKS_PER_SECOND);
    return static_cast<uint8_t>(seconds >> (8 * (3 - (offset & 3))));
}

void ZX8302::write_link(uint8_t value, IPC& ipc) {
    // The start bit pulls COMDATA low, which the IPC is waiting for; then the
    // data bit goes out and the IPC's two COMCTL pulses latch the line for
    // the CPU (MAME's transmit_ipc_data and comctl_w). The JS ROM writes
    // %11x0 to send x and %1110 to listen ($2F7C, $2F9C).
    if ((value & 0x01) != 0) return;
    comdata_ = ipc.exchange((value & 0x02) != 0) ? 1 : 0;
}

} // namespace ql
