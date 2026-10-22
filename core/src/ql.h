#pragma once
// The Sinclair QL around its 68008: the memory map the CPU sees, and running
// the machine a frame at a time. The display (ZX8301), the peripheral chip
// (ZX8302) and the IPC join it in later tickets; until then the I/O area
// reads as nothing.
//
// The map is MAME's QL driver's (src/mame/sinclair/ql.cpp, BSD-3-Clause), and
// docs/machine.md describes it.

#include "m68008.h"
#include "zx8301.h"
#include "zx8302.h"

#include <cstdint>
#include <vector>

namespace ql {

class QL : public Bus {
public:
    /// The system ROM's image -- up to 48K, the rest reading as 0 -- and the
    /// RAM size in K.
    QL(std::vector<uint8_t> rom, unsigned ram_kb);

    /// Power-on, or the reset button: the CPU's reset sequence.
    void reset();
    /// Sets the real-time clock, in seconds since 1 January 1961 (see
    /// rtc_from_unix). The server sets it from the host clock at power-on and
    /// reset; after that it moves only with emulated time.
    void set_rtc(uint32_t seconds) { zx8302_.set_rtc(seconds, cpu_.clock()); }
    /// One instruction, with whatever the machine does around it: the frame
    /// interrupt raised as each frame begins, and the IPL lines set.
    void step();
    /// Runs whole instructions to the end of the frame `frames` on from the
    /// current one. Frames are counted from power-on, frame k ending at
    /// k x 150,000 clocks (7.5 MHz at 50 Hz).
    void run_frames(unsigned frames);

    /// A byte of memory as the CPU would read it, without side effects.
    uint8_t peek(uint32_t address) const;
    /// Writes a byte of RAM directly, as a debugger or a test does: ROM and
    /// I/O are left alone.
    void poke(uint32_t address, uint8_t value);

    /// The screen as the ZX8301 draws it from RAM now.
    Screen screen() const;

    M68008& cpu() { return cpu_; }
    const M68008& cpu() const { return cpu_; }

    uint8_t read(const BusCycle& cycle) override;
    void write(const BusCycle& cycle, uint8_t data) override;

private:
    std::vector<uint8_t> rom_;
    std::vector<uint8_t> ram_;
    ZX8301 display_;
    ZX8302 zx8302_;
    /// When the next frame begins, with its vertical sync.
    uint64_t next_vsync_ = 0;
    M68008 cpu_;
};

} // namespace ql
