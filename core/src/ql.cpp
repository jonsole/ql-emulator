#include "ql.h"

#include <stdexcept>
#include <utility>

namespace ql {

namespace {

/// The system ROM: $00000-$0BFFF.
constexpr uint32_t ROM_SIZE = 0x0C000;
/// The ZX8301's display control register, write-only (MAME's QL driver).
constexpr uint32_t ZX8301_CONTROL = 0x18063;
/// The ZX8302's real-time clock, four bytes, read-only here: MAME ignores
/// writes to it, and so does this for now.
constexpr uint32_t ZX8302_RTC = 0x18000;
/// The ZX8302's interrupt register (MAME's QL driver).
constexpr uint32_t ZX8302_INTERRUPT = 0x18021;
/// The ZX8302's status register, read-only here; its IPC link bits so far
/// (MAME's QL driver).
constexpr uint32_t ZX8302_STATUS = 0x18020;
/// The IPC link register, write-only (MAME's QL driver; the JS ROM's $2F6E).
constexpr uint32_t ZX8302_LINK = 0x18003;
/// IPL1L alone, which is all the ZX8302 drives, is interrupt level 2 on the
/// QL's 48-pin 68008 (User's Manual 6.3.2: levels 2, 5 and 7 only).
constexpr unsigned ZX8302_LEVEL = 2;
/// RAM starts at $20000, the screen at its foot.
constexpr uint32_t RAM_BASE = 0x20000;
/// The QL's 48-pin 68008 drives A0-A19 only: the CPU's 24-bit addresses wrap
/// at 1 MB.
constexpr uint32_t ADDRESS_MASK = 0xFFFFF;
/// The 68008 at 7.5 MHz and 50 frames a second.
constexpr uint64_t CLOCKS_PER_FRAME = 7'500'000 / 50;
/// How often the IPC scans its keyboard, in 68008 clocks. Its idle loop --
/// the scan, the serial housekeeping and the look for a command, $500-$207 --
/// is about 308 machine cycles by the opcode timings in the IPC ROM's
/// disassembly; at 11 MHz / 15, 733,333 machine cycles a second, that is
/// 308 x 7,500,000 / 733,333 = 3,150 clocks, some 2,400 scans a second.
constexpr uint64_t CLOCKS_PER_IPC_SCAN = 3'150;

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
    // MAME resets every device with the machine, the 8049 with the 68008.
    ipc_.reset();
    cpu_.reset();
}

void QL::step() {
    // Each frame begins with the display's vertical sync, raising the frame
    // interrupt: at power-on, and every 150,000 clocks after it.
    while (cpu_.clock() >= next_vsync_) {
        zx8302_.vsync();
        next_vsync_ += CLOCKS_PER_FRAME;
    }
    // The IPC scans its keyboard on its own clock, all the time it is not
    // answering a command; its answers take no time here.
    while (cpu_.clock() >= next_ipc_scan_) {
        ipc_.scan();
        next_ipc_scan_ += CLOCKS_PER_IPC_SCAN;
    }
    cpu_.set_interrupt_level(zx8302_.interrupt_pending() ? ZX8302_LEVEL : 0);
    cpu_.step();
}

void QL::run_frames(unsigned frames) {
    // Frames are counted from power-on, frame k ending at k x 150,000 clocks,
    // so that an instruction running over a frame's end does not move every
    // frame after it.
    uint64_t end = (cpu_.clock() / CLOCKS_PER_FRAME + frames) * CLOCKS_PER_FRAME;
    while (cpu_.clock() < end) step();
}

uint8_t QL::peek(uint32_t address) const {
    address &= ADDRESS_MASK;
    if (address < ROM_SIZE) return rom_[address];
    if (address >= RAM_BASE && address - RAM_BASE < ram_.size()) return ram_[address - RAM_BASE];
    if (address == ZX8302_INTERRUPT) return zx8302_.read_interrupt();
    if (address == ZX8302_STATUS) return zx8302_.read_status();
    if (address >= ZX8302_RTC && address < ZX8302_RTC + 4) return zx8302_.read_rtc(address - ZX8302_RTC, cpu_.clock());
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
    if (address == ZX8302_INTERRUPT) {
        zx8302_.write_interrupt(data);
        return;
    }
    if (address == ZX8302_LINK) {
        zx8302_.write_link(data, ipc_);
        return;
    }
    if (address >= RAM_BASE && address - RAM_BASE < ram_.size()) ram_[address - RAM_BASE] = data;
}

} // namespace ql
