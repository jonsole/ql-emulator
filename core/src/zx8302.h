#pragma once
// The ZX8302: the QL's peripheral chip. So far its interrupts -- the frame
// interrupt, raised by the display's vertical sync -- and the interrupt
// register. The real-time clock, the IPC link, the microdrives and the
// network come with their own tickets.
//
// From MAME's ZX8302 (src/mame/sinclair/zx8302.cpp and .h, BSD-3-Clause).

#include <cstdint>

namespace ql {

class ZX8302 {
public:
    // The interrupt register's pending bits (MAME's INT_*): gap, interface,
    // transmit, frame, external. Only the frame interrupt is raised so far.
    static constexpr uint8_t INT_GAP = 0x01;
    static constexpr uint8_t INT_INTERFACE = 0x02;
    static constexpr uint8_t INT_TRANSMIT = 0x04;
    static constexpr uint8_t INT_FRAME = 0x08;
    static constexpr uint8_t INT_EXTERNAL = 0x10;

    /// The display's vertical sync: the frame interrupt.
    void vsync() { pending_ |= INT_FRAME; }

    /// A read of the interrupt register: the pending interrupts, without the
    /// mask (MAME's irq_status_r).
    uint8_t read_interrupt() const { return pending_; }

    /// A write to the interrupt register: the low five bits clear those
    /// pending, the top three set the mask (gap, interface, transmit).
    void write_interrupt(uint8_t value);

    /// IPL1L: held while anything is pending, which on the QL's 48-pin 68008
    /// is interrupt level 2.
    bool interrupt_pending() const { return pending_ != 0; }

    /// Sets the real-time clock to `seconds`, at CPU clock `clock`. It then
    /// counts one second per 7,500,000 clocks: emulated time, not the host's
    /// (docs/plan.md, The machine).
    void set_rtc(uint32_t seconds, uint64_t clock) {
        rtc_seconds_ = seconds;
        rtc_set_at_ = clock;
    }
    /// A byte of the real-time clock, $18000-$18003: `offset` 0 is the most
    /// significant.
    uint8_t read_rtc(unsigned offset, uint64_t clock) const;

private:
    uint8_t pending_ = 0;
    uint8_t mask_ = 0;
    uint32_t rtc_seconds_ = 0;
    uint64_t rtc_set_at_ = 0;
};

/// The real-time clock's count for a Unix time: seconds since 1 January 1961,
/// 3,287 days before 1970 (MAME's RTC_BASE_ADJUST).
constexpr uint32_t rtc_from_unix(int64_t unix_seconds) {
    return static_cast<uint32_t>(unix_seconds + 283'996'800);
}

} // namespace ql
