#include "ql.h"

#include <stdexcept>
#include <utility>

namespace ql {

namespace {

/// The system ROM: $00000-$0BFFF.
constexpr uint32_t ROM_SIZE = 0x0C000;
/// The ZX8301's display control register, write-only (MAME's QL driver).
constexpr uint32_t ZX8301_CONTROL = 0x18063;
/// RAM starts at $20000, the screen at its foot.
constexpr uint32_t RAM_BASE = 0x20000;
/// The QL's 48-pin 68008 drives A0-A19 only: the CPU's 24-bit addresses wrap
/// at 1 MB.
constexpr uint32_t ADDRESS_MASK = 0xFFFFF;
/// The 68008 at 7.5 MHz and 50 frames a second.
constexpr uint64_t CLOCKS_PER_FRAME = 7'500'000 / 50;

} // namespace

QL::QL(std::vector<uint8_t> rom, unsigned ram_kb) : rom_(std::move(rom)), ram_(ram_kb * 1024u, 0), cpu_(*this) {
    // The plan's two sizes: the standard 128K, and 640K with the 512K
    // expansion area from $40000 filled.
    if (ram_kb != 128 && ram_kb != 640) throw std::invalid_argument("QL: RAM must be 128K or 640K");
    // The system ROM fills $00000-$0BFFF. A larger image is not one; a
    // smaller one -- a test's -- leaves the rest reading as 0.
    if (rom_.size() > ROM_SIZE) throw std::invalid_argument("QL: the system ROM is at most 48K");
    rom_.resize(ROM_SIZE, 0);
}

void QL::reset() {
    cpu_.reset();
}

void QL::run_frames(unsigned frames) {
    // Frames are counted from power-on, frame k ending at k x 150,000 clocks,
    // so that an instruction running over a frame's end does not move every
    // frame after it.
    uint64_t end = (cpu_.clock() / CLOCKS_PER_FRAME + frames) * CLOCKS_PER_FRAME;
    while (cpu_.clock() < end) cpu_.step();
}

uint8_t QL::peek(uint32_t address) const {
    address &= ADDRESS_MASK;
    if (address < ROM_SIZE) return rom_[address];
    if (address >= RAM_BASE && address - RAM_BASE < ram_.size()) return ram_[address - RAM_BASE];
    // Nothing answers: 0, as in MAME's QL driver.
    return 0;
}

void QL::poke(uint32_t address, uint8_t value) {
    address &= ADDRESS_MASK;
    if (address >= RAM_BASE && address - RAM_BASE < ram_.size()) ram_[address - RAM_BASE] = value;
}

Screen QL::screen() const {
    return display_.render(ram_, cpu_.clock() / CLOCKS_PER_FRAME);
}

uint8_t QL::read(const BusCycle& cycle) {
    return peek(cycle.address);
}

void QL::write(const BusCycle& cycle, uint8_t data) {
    uint32_t address = cycle.address & ADDRESS_MASK;
    if (address == ZX8301_CONTROL) {
        display_.write_control(data);
        return;
    }
    if (address >= RAM_BASE && address - RAM_BASE < ram_.size()) ram_[address - RAM_BASE] = data;
}

} // namespace ql
