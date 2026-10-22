#pragma once
// The ZX8301: the QL's display. It draws the screen from RAM onto the 512x256
// canvas in either of the QL's two modes. Contention -- the ZX8301 holding
// the CPU off the bus while it reads -- is a later ticket.
//
// The screen encoding is MAME's (src/mame/sinclair/zx8301.cpp, BSD-3-Clause)
// and sQLux's (src/SDL2screen.c, Zlib); docs/machine.md describes it.

#include <cstdint>
#include <vector>

namespace ql {

/// The canvas: 512x256 in both modes, mode 8's pixels drawn two dots wide.
constexpr unsigned SCREEN_WIDTH = 512;
constexpr unsigned SCREEN_HEIGHT = 256;

/// A frame as 0xRRGGBB, SCREEN_WIDTH x SCREEN_HEIGHT, row by row.
using Screen = std::vector<uint32_t>;

class ZX8301 {
public:
    /// The display control register, write-only, at $18063: bit 1 turns the
    /// display off, bit 3 selects mode 8, bit 7 the second screen.
    void write_control(uint8_t value) { control_ = value; }

    /// Draws the screen from RAM, `ram` being the QL's RAM from $20000, in
    /// frame `frame` counted from power-on (for mode 8's flash).
    Screen render(const std::vector<uint8_t>& ram, uint64_t frame) const;

private:
    uint8_t control_ = 0;
};

} // namespace ql
