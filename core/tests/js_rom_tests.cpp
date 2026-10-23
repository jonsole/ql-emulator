// The JS ROM boots: from reset to its F1/F2 choice, and on F1 to the
// SuperBASIC prompt, where a line typed through the IPC is run.
//
// The JS ROM is supplied by whoever runs the tests, in roms/js.rom; without it
// the tests skip (exit 77, which CTest reports as skipped). Nothing here is
// captured from a run: the windows' places and colours are read from the ROM's
// own window table, the text from its own messages and font, and what each
// means from the JS ROM disassembly on Dilwyn Jones's disassemblies page
// (jsrom.zip), whose labels are the ROM's addresses.

#include "ql.h"
#include "test_main.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

/// The exit code CTest is told means skipped.
constexpr int EXIT_SKIPPED = 77;

/// The window definitions SuperBASIC opens #0, #1 and #2 with on a monitor
/// (L04B72 in BASIC1), and the window the F1/F2 choice is printed in (L04BD2,
/// "window for extensions message"). The disassembly's monitor #2 has its
/// paper and ink the wrong way round; the ROM's bytes are 07 02.
constexpr uint32_t MONITOR_0 = 0x4B72;
constexpr uint32_t MONITOR_1 = 0x4B7E;
constexpr uint32_t START_WINDOW = 0x4BD2;
/// The ROM's character font (L0AD9A, CHARFOUN_T.A): a first character, the
/// number of characters less one, then 9 bytes to each.
constexpr uint32_t FONT = 0xAD9A;

/// A QL colour number as the screen shows it in `mode`: in mode 8 the
/// colour is green x 4 + red x 2 + blue; mode 4 has no blue, so 1 is black,
/// 3 red, 5 green and 6 white (docs/machine.md, The display).
uint32_t colour(int mode, unsigned number) {
    number &= 7;
    if (mode == 4) {
        number &= 6;
        if (number == 6) number = 7;
    }
    uint32_t rgb = 0;
    if (number & 4) rgb |= 0x00FF00;
    if (number & 2) rgb |= 0xFF0000;
    if (number & 1) rgb |= 0x0000FF;
    return rgb;
}

std::vector<uint8_t> js_rom;

unsigned word_at(uint32_t address) { return unsigned(js_rom[address]) << 8 | js_rom[address + 1]; }

/// A window definition as UT.WINDW takes it (L04B72's comments): border
/// colour, border width, paper, ink, then width, height and origin in pixels.
struct Window {
    unsigned paper;
    unsigned ink;
    /// Where its first character goes: inside the border, which is its width
    /// deep at the top and twice its width at the sides (the QL User Guide,
    /// BORDER).
    int text_x;
    int text_y;
};

Window window_at(uint32_t address) {
    unsigned border = js_rom[address + 1];
    Window window;
    window.paper = js_rom[address + 2];
    window.ink = js_rom[address + 3];
    window.text_x = static_cast<int>(word_at(address + 8) + 2 * border);
    window.text_y = static_cast<int>(word_at(address + 10) + border);
    return window;
}

/// Reads `length` characters of `window`'s line `line` off the screen, as the
/// ROM draws them at the standard size ($2840-$2956): a cell 6 pixels wide
/// and 10 high, its top row paper and the font's 9 rows below, bits 7-2 of
/// each the 6 pixels from the left. A mode-8 pixel is two dots wide on the
/// 512-dot screen, so there a cell is 12 dots. A cell matching no character
/// reads as '?'.
std::string read_text(const ql::Screen& screen, int mode, const Window& window, int line, int length) {
    uint32_t ink = colour(mode, window.ink);
    int dots = mode == 8 ? 2 : 1;
    unsigned first = js_rom[FONT];
    unsigned count = js_rom[FONT + 1] + 1u;
    std::string text;
    for (int i = 0; i < length; i++) {
        int x = window.text_x + 6 * dots * i;
        int y = window.text_y + 10 * line;
        uint8_t rows[10] = {};
        for (int row = 0; row < 10; row++) {
            for (int column = 0; column < 6; column++) {
                if (screen[size_t(y + row) * ql::SCREEN_WIDTH + size_t(x + dots * column)] == ink) {
                    rows[row] = static_cast<uint8_t>(rows[row] | 0x80 >> column);
                }
            }
        }
        char found = '?';
        for (unsigned c = 0; c < count && rows[0] == 0; c++) {
            bool match = true;
            for (int row = 0; row < 9; row++) {
                if ((js_rom[FONT + 2 + 9 * c + unsigned(row)] & 0xFC) != rows[row + 1]) match = false;
            }
            if (match) {
                found = static_cast<char>(first + c);
                break;
            }
        }
        text += found;
    }
    return text;
}

/// Runs a frame at a time, up to `limit` frames, until `done` holds.
template <typename Done>
bool run_until(ql::QL& machine, unsigned limit, Done done) {
    for (unsigned frame = 0; frame < limit; frame++) {
        if (done()) return true;
        machine.run_frames(1);
    }
    return done();
}

/// Types a key: held 3 frames and let go for 3, so that the IPC has it
/// whole and alone. A shifted key has SHIFT held a frame either side, as
/// the IPC ignores a key whose modifiers changed while it was down.
void type(ql::QL& machine, ql::QLKey key, bool shifted = false) {
    if (shifted) {
        machine.press_key(ql::QLKey::Shift);
        machine.run_frames(1);
    }
    machine.press_key(key);
    machine.run_frames(3);
    machine.release_key(key);
    machine.run_frames(3);
    if (shifted) {
        machine.release_key(ql::QLKey::Shift);
        machine.run_frames(1);
    }
}

} // namespace

// From reset, the ROM tests and clears RAM, puts the display in mode 8 ($8
// to $18063, in SYSTEM's start-up), and asks for F1 or F2 in its start window
// (BASIC1, GETMOD): message -$18, "F1...monitor" and "F2...TV" on two lines,
// in the window's ink. F1 asks for mode 4 (INIMON, D6 = 0) and the monitor
// windows, tries BOOT and MDV1_BOOT -- with no microdrive, not found -- and
// SuperBASIC waits for a line on #0, with its cursor at #0's first cell,
// otherwise blank. "print 7*6", typed and entered, is echoed on #0 in its
// ink, and 42 is printed at the top of #1, in its ink: both read as mode-4
// characters, 6 dots wide.
TEST(the_js_rom_boots_to_superbasic_and_runs_a_line) {
    ql::QL machine(js_rom, 128);
    machine.reset();
    Window start = window_at(START_WINDOW);
    bool asked = run_until(machine, 500, [&] { return read_text(machine.screen(), 8, start, 0, 12) == "F1...monitor"; });
    CHECK(asked);
    CHECK_EQ(read_text(machine.screen(), 8, start, 1, 7), std::string("F2...TV"));

    machine.press_key(ql::QLKey::F1);
    machine.run_frames(5);
    machine.release_key(ql::QLKey::F1);
    Window command = window_at(MONITOR_0);
    bool waiting = run_until(machine, 1000, [&] {
        ql::Screen screen = machine.screen();
        for (int row = 0; row < 10; row++) {
            for (int column = 0; column < 6; column++) {
                size_t at = size_t(command.text_y + row) * ql::SCREEN_WIDTH + size_t(command.text_x + column);
                if (screen[at] != colour(4, command.paper)) return true;
            }
        }
        return false;
    });
    CHECK(waiting);

    using K = ql::QLKey;
    const K line[] = {K::P, K::R, K::I, K::N, K::T, K::Space, K::Num7};
    for (K key : line) type(machine, key);
    type(machine, K::Num8, true);   // '*', SHIFT 8
    type(machine, K::Num6);
    type(machine, K::Enter);
    Window output = window_at(MONITOR_1);
    bool printed = run_until(machine, 100, [&] { return read_text(machine.screen(), 4, output, 0, 2) == "42"; });
    CHECK(printed);
    CHECK_EQ(read_text(machine.screen(), 4, command, 0, 9), std::string("print 7*6"));
}

int main() {
    std::filesystem::path path = std::filesystem::path(QL_PROJECT_ROOT) / "roms" / "js.rom";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("skipped: %s not found -- the JS ROM is supplied by whoever runs the tests\n",
                    path.lexically_normal().string().c_str());
        return EXIT_SKIPPED;
    }
    js_rom.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return qltest::run_all();
}
