#pragma once
// The Sinclair QL around its 68008: the memory map the CPU sees, and running
// the machine a frame at a time. The display (ZX8301), the peripheral chip
// (ZX8302) and the IPC join it in later tickets; until then the I/O area
// reads as nothing.
//
// The map is MAME's QL driver's (src/mame/sinclair/ql.cpp, BSD-3-Clause), and
// docs/machine.md describes it.

#include "m68008.h"

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
    /// Runs whole instructions to the end of the frame `frames` on from the
    /// current one. Frames are counted from power-on, frame k ending at
    /// k x 150,000 clocks (7.5 MHz at 50 Hz).
    void run_frames(unsigned frames);

    /// A byte of memory as the CPU would read it, without side effects.
    uint8_t peek(uint32_t address) const;

    M68008& cpu() { return cpu_; }

    uint8_t read(const BusCycle& cycle) override;
    void write(const BusCycle& cycle, uint8_t data) override;

private:
    std::vector<uint8_t> rom_;
    std::vector<uint8_t> ram_;
    M68008 cpu_;
};

} // namespace ql
