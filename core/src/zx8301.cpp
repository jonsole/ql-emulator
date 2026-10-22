#include "zx8301.h"

namespace ql {

namespace {

/// Each line is 128 bytes: 64 words.
constexpr unsigned BYTES_PER_LINE = 128;

// The control register's bits (MAME's ZX8301): display off, mode 8, and the
// second screen, 32K above the first.
constexpr uint8_t CONTROL_DISPLAY_OFF = 0x02;
constexpr uint8_t CONTROL_MODE_8 = 0x08;
constexpr uint8_t CONTROL_SECOND_SCREEN = 0x80;
constexpr uint32_t SECOND_SCREEN_OFFSET = 0x8000;

/// Frames in each phase of mode 8's flash (sQLux).
constexpr uint64_t FRAMES_PER_FLASH_PHASE = 32;

/// The QL's eight colours, by green x 4 + red x 2 + blue.
constexpr uint32_t PALETTE[8] = {
    0x000000,   // black
    0x0000FF,   // blue
    0xFF0000,   // red
    0xFF00FF,   // magenta
    0x00FF00,   // green
    0x00FFFF,   // cyan
    0xFFFF00,   // yellow
    0xFFFFFF,   // white
};

/// Mode 4's four colours, by green x 2 + red: black, red, green, white.
constexpr unsigned MODE_4_COLOURS[4] = {0, 2, 4, 7};

} // namespace

Screen ZX8301::render(const std::vector<uint8_t>& ram, uint64_t frame) const {
    Screen screen(SCREEN_WIDTH * SCREEN_HEIGHT, 0);
    if (control_ & CONTROL_DISPLAY_OFF) return screen;
    bool mode_8 = control_ & CONTROL_MODE_8;
    uint32_t base = (control_ & CONTROL_SECOND_SCREEN) ? SECOND_SCREEN_OFFSET : 0;
    // The flash phase changes every 32 frames from power-on, off first:
    // sQLux's 50 Hz frame counter, whose bit 5 is the phase. MAME instead
    // toggles at 2 Hz from a timer its own notes mark for removal; a divider
    // on the frame count is how the hardware would do it.
    bool flash_on = (frame / FRAMES_PER_FLASH_PHASE) & 1;
    for (unsigned y = 0; y < SCREEN_HEIGHT; y++) {
        uint32_t line = base + y * BYTES_PER_LINE;
        uint32_t* out = &screen[y * SCREEN_WIDTH];
        // Flashing never carries over from one line to the next.
        bool flashing = false;
        uint32_t flash_colour = 0;
        for (unsigned word = 0; word < BYTES_PER_LINE / 2; word++) {
            uint8_t even = ram[line + 2 * word];
            uint8_t odd = ram[line + 2 * word + 1];
            if (!mode_8) {
                // Mode 4: a pixel per bit, bit 7 leftmost; green in the even
                // byte and red in the odd give black, red, green or white.
                for (unsigned bit = 0; bit < 8; bit++) {
                    unsigned g = (even >> (7 - bit)) & 1;
                    unsigned r = (odd >> (7 - bit)) & 1;
                    *out++ = PALETTE[MODE_4_COLOURS[g * 2 + r]];
                }
                continue;
            }
            // Mode 8: a pixel per bit pair, from bits 7 and 6 -- green and
            // flash in the even byte, red and blue in the odd -- each drawn
            // two dots wide. A flash bit starts flashing in that pixel's
            // colour, and the next one ends it, after itself; in the on
            // phase the pixels while flashing show the starting colour
            // (sQLux's rule, which MAME's matches).
            for (unsigned pair = 0; pair < 4; pair++) {
                unsigned shift = 6 - 2 * pair;
                unsigned g = (even >> (shift + 1)) & 1;
                unsigned f = (even >> shift) & 1;
                unsigned r = (odd >> (shift + 1)) & 1;
                unsigned b = (odd >> shift) & 1;
                uint32_t colour = PALETTE[g * 4 + r * 2 + b];
                uint32_t shown = flashing && flash_on ? flash_colour : colour;
                *out++ = shown;
                *out++ = shown;
                if (f) {
                    if (!flashing) flash_colour = colour;
                    flashing = !flashing;
                }
            }
        }
    }
    return screen;
}

} // namespace ql
