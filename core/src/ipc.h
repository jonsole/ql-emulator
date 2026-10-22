#pragma once
// The IPC: the QL's second processor, an Intel 8049 that scans the keyboard,
// makes the sound and receives from the serial ports. It is emulated at its
// command protocol, not as an 8049 (docs/plan.md, The machine): the 68008
// sends it a command a bit at a time through the ZX8302's link register and
// reads the reply the same way.
//
// What each command does is read off the IPC ROM's disassembly (ipc8049.txt,
// JV 970715, from Dilwyn Jones's disassemblies page); addresses in the
// comments are the 8049's. The command numbers agree with Minerva's
// inc/ipcmd. docs/ipc.md describes what is covered and what is not.

#include <cstdint>
#include <vector>

namespace ql {

/// The QL's keys, each numbered row x 8 + column by where it sits in the
/// keyboard matrix: the row is the KEYROW row, the 8049's column output KBO0-7
/// that drives it, and the column the bit of the row read back on KBI0-7.
/// The matrix is MAME's QL driver's (INPUT_PORTS_START(ql), ports Y0-Y7),
/// which is the QL User Guide's KEYROW table.
enum class QLKey : uint8_t {
    F4 = 0, F1 = 1, Num5 = 2, F2 = 3, F3 = 4, F5 = 5, Num4 = 6, Num7 = 7,
    Enter = 8, Left = 9, Up = 10, Esc = 11, Right = 12, Backslash = 13, Space = 14, Down = 15,
    RightBracket = 16, Z = 17, Period = 18, C = 19, B = 20, Pound = 21, M = 22, Quote = 23,
    LeftBracket = 24, CapsLock = 25, K = 26, S = 27, F = 28, Equals = 29, G = 30, Semicolon = 31,
    L = 32, Num3 = 33, H = 34, Num1 = 35, A = 36, P = 37, D = 38, J = 39,
    Num9 = 40, W = 41, I = 42, Tab = 43, R = 44, Minus = 45, Y = 46, O = 47,
    Num8 = 48, Num2 = 49, Num6 = 50, Q = 51, E = 52, Num0 = 53, T = 54, U = 55,
    Shift = 56, Ctrl = 57, Alt = 58, X = 59, V = 60, Slash = 61, N = 62, Comma = 63,
};

class IPC {
public:
    /// Power-on, or command 0: the IPC starts again from IPCinit ($009).
    void reset();

    /// Presses or lets go of a key in the matrix the IPC scans.
    void set_key(QLKey key, bool down);
    /// One pass of the IPC's keyboard scan ($500-$5BE).
    void scan();

    /// One bit over the link. `bit` is what the ZX8302 drives on COMDATA for
    /// the bit -- the CPU's bit when it sends, 1 when it listens -- and the
    /// result is the line's level: COMDATA is wired AND, so a 0 from either
    /// end wins.
    bool exchange(bool bit);

private:
    /// What the IPC is waiting to hear: a command's four bits, or its
    /// parameters.
    void received();
    /// Carries out `command_` with its parameters, `in_`.
    void execute();
    /// Queues the low `bits` bits of `value` to send, most significant first.
    void reply(unsigned value, int bits);

    /// The command whose parameters are arriving, or -1 while waiting for one.
    int command_ = -1;
    unsigned in_ = 0;
    int in_bits_ = 0;
    int in_needed_ = 4;
    /// The reply still to send, a bit to each element, and how far it has got.
    std::vector<uint8_t> out_;
    size_t out_next_ = 0;

    /// The keys down, a byte to each KEYROW row, bit n for column n. They are
    /// the keyboard, not the IPC, so a reset leaves them alone.
    uint8_t matrix_[8] = {};

    // The IPC's own state, named for where its ROM keeps it.
    /// IPCSTAT ($20): bit 0 a key buffered, bit 2 SHIFT, CTRL or ALT changed,
    /// bit 3 a key held.
    uint8_t status_ = 0;
    /// The modifier code of the last scan ($28): SHIFT 4, CTRL 2, ALT 1.
    uint8_t modifiers_ = 0;
    /// The key being debounced, $FF for none ($29), and how many scans it has
    /// been seen for since ($2A).
    uint8_t key_ = 0xFF;
    uint8_t count_ = 0;
    /// The keyboard buffer ($2C-$39): up to 7 keys, each a modifier nibble --
    /// bit 3 lost keys, then SHIFT, CTRL, ALT -- and a keycode.
    struct Keydef {
        uint8_t modifiers;
        uint8_t keycode;
    };
    std::vector<Keydef> buffer_;
    /// The random number's seed: $25 low, $26 high.
    uint16_t seed_ = 0;
    /// KBDSEED ($27): one more each scan.
    uint8_t scan_count_ = 0;
    /// The 8049's register R4 as the last scan left it, which the random
    /// number's step uses without setting ($73A): the modifier code ($50F),
    /// or a key's column + 1 once one is found ($5DC-$5E8).
    uint8_t r4_ = 0;
};

} // namespace ql
