#include "ipc.h"

namespace ql {

namespace {

/// The keyboard buffer's size: $2C-$39, two bytes to a key.
constexpr size_t KEYBOARD_BUFFER_KEYS = 7;

/// The bits of parameters each command reads before it acts: the jump table
/// at $20C and the handlers it leads to.
int parameter_bits(int command) {
    switch (command) {
    case 9: return 4;    // KEYROW: the row ($276)
    case 10: return 64;  // sound: 8 bytes, pitches, interval, duration, step ($300)
    case 12: return 4;   // P2.3: a nibble ($2C3)
    case 13: return 4;   // baud rate: a nibble ($2CF)
    case 15: return 8;   // test: a byte ($2F0)
    default: return 0;
    }
}

} // namespace

void IPC::reset() {
    command_ = -1;
    in_ = 0;
    in_bits_ = 0;
    in_needed_ = 4;
    out_.clear();
    out_next_ = 0;
    // IPCinit clears its RAM, $20-$7F, then sets the key being debounced to
    // none ($009-$02D).
    status_ = 0;
    modifiers_ = 0;
    key_ = 0xFF;
    count_ = 0;
    buffer_.clear();
    seed_ = 0;
    scan_count_ = 0;
    r4_ = 0;
}

void IPC::set_key(QLKey key, bool down) {
    unsigned row = static_cast<unsigned>(key) >> 3;
    uint8_t column = static_cast<uint8_t>(1u << (static_cast<unsigned>(key) & 7));
    if (down) {
        matrix_[row] = static_cast<uint8_t>(matrix_[row] | column);
    } else {
        matrix_[row] = static_cast<uint8_t>(matrix_[row] & ~column);
    }
}

void IPC::scan() {
    scan_count_++;
    // Row 7 first, for SHIFT, CTRL and ALT in its bits 0-2, made into the
    // modifier code by the table at $5C0. A change of them is all this scan
    // does ($510-$51F).
    static const uint8_t MODIFIER_CODE[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    uint8_t modifiers = MODIFIER_CODE[matrix_[7] & 7];
    r4_ = modifiers;
    if (modifiers != modifiers_) {
        modifiers_ = modifiers;
        status_ = static_cast<uint8_t>((status_ | 0x04) & ~0x08);
        return;
    }

    // Then the other keys, row 7's bits 3-7 and rows 6 down to 0, each row's
    // bit 7 first ($521-$533, $5DC-$5F8).
    int keycode = -1;
    for (int row = 7; row >= 0; row--) {
        uint8_t bits = row == 7 ? static_cast<uint8_t>(matrix_[7] & 0xF8) : matrix_[row];
        for (int column = 7; column >= 0; column--) {
            if (((bits >> column) & 1) == 0) continue;
            if (keycode >= 0) {
                // A second key, in this row or another: the IPC has no
                // rollover, and forgets the key it was debouncing ($5D9,
                // $53F-$54A).
                status_ = static_cast<uint8_t>(status_ & ~0x08);
                key_ = 0xFF;
                count_ = 0;
                return;
            }
            keycode = (7 - row) << 3 | column;
            r4_ = static_cast<uint8_t>(column + 1);
        }
    }
    if (keycode < 0) {
        // Nothing down: no key is being debounced, and none held ($539-$54C).
        status_ = static_cast<uint8_t>(status_ & ~0x0C);
        key_ = 0xFF;
        count_ = 0;
        return;
    }

    // One key, ignored while SHIFT, CTRL or ALT have changed since a scan
    // last found nothing down ($54E-$553). So a key already down when they
    // change is shut out until it is let go.
    if (status_ & 0x04) return;
    // Otherwise it has to be seen on 6 scans running before it is buffered,
    // and from the 7th it is held ($555-$5B8).
    if (keycode != key_) {
        key_ = static_cast<uint8_t>(keycode);
        count_ = 0;
        return;
    }
    if (count_ < 4) {
        count_++;
        return;
    }
    if (count_ > 4) {
        status_ |= 0x08;
        return;
    }
    count_ = 5;
    if (buffer_.size() == KEYBOARD_BUFFER_KEYS) {
        // Full: the key is lost, and the last key buffered says so in bit 3
        // of its modifier nibble ($5B0-$5B5). A later 7th key, after the
        // buffer has been read, starts without it.
        buffer_.back().modifiers |= 0x08;
        return;
    }
    buffer_.push_back(Keydef{modifiers_, key_});
    status_ |= 0x01;
    // The moment a key is typed stirs the random number ($5A7-$5AC).
    seed_ = static_cast<uint16_t>(seed_ + scan_count_);
}

bool IPC::exchange(bool bit) {
    // A reply goes out a bit at a time, the IPC driving COMDATA with it
    // ($762); the CPU listens by driving 1, so the line shows the IPC's bit.
    if (out_next_ < out_.size()) {
        bool ipc_bit = out_[out_next_++] != 0;
        if (out_next_ == out_.size()) {
            out_.clear();
            out_next_ = 0;
        }
        return bit && ipc_bit;
    }
    // Otherwise the IPC is listening ($74F), leaving the line to the ZX8302.
    in_ = in_ << 1 | (bit ? 1u : 0u);
    in_bits_++;
    if (in_bits_ == in_needed_) received();
    return bit;
}

void IPC::received() {
    if (command_ < 0) {
        command_ = static_cast<int>(in_);
        in_needed_ = parameter_bits(command_);
        in_ = 0;
        in_bits_ = 0;
        if (in_needed_ > 0) return;
    }
    execute();
    command_ = -1;
    in_ = 0;
    in_bits_ = 0;
    in_needed_ = 4;
}

void IPC::execute() {
    switch (command_) {
    case 0:
        // Reset ($21C): IPCinit again.
        reset();
        break;
    case 1: {
        // Status ($228-$249): IPCSTAT, with bit 0 set while a key is held as
        // well as when one is buffered. Bit 6 is the P2.6 pin, which nothing
        // drives; MAME reads it as 0, and so does this. Bits 4 and 5, serial
        // input waiting, stay 0 with no serial input emulated.
        uint8_t status = static_cast<uint8_t>(status_ & 0xCF);
        if (status & 0x08) status |= 0x01;
        reply(status, 8);
        break;
    }
    case 8: {
        // Read keyboard ($24D-$274): a nibble, bit 3 held and bits 2-0 how
        // many keys follow, then each key's modifier nibble and keycode. The
        // buffer is emptied and the buffered bit cleared.
        reply(static_cast<unsigned>((status_ & 0x08) | buffer_.size()), 4);
        for (const Keydef& key : buffer_) {
            reply(key.modifiers, 4);
            reply(key.keycode, 8);
        }
        buffer_.clear();
        status_ = static_cast<uint8_t>(status_ & ~0x01);
        break;
    }
    case 9:
        // KEYROW ($276-$285): the row's column output driven and the matrix
        // read straight back. Only the nibble's low three bits select it.
        reply(matrix_[in_ & 7], 8);
        break;
    case 6:
    case 7:
        // Read a serial channel ($289-$2C1): a count of the bytes to follow
        // in the low 6 bits, then the bytes. No serial input is emulated, so
        // there are none.
        reply(0x00, 8);
        break;
    case 10:
    case 11:
        // Sound and kill sound: taken and ignored until Stage 6. The real IPC
        // would set status bit 1 while a sound plays ($31A).
        break;
    case 12:
        // P2.3 low would hold IPL1L, a level-2 interrupt that never ends
        // ($2C7); Minerva's inc/ipcmd calls it a side effect of microdrive
        // reduced sensitivity. Not emulated.
        break;
    case 2:
    case 3:
    case 4:
    case 5:
    case 13:
        // Opening and closing the serial channels, and the baud rate: they
        // matter only to serial input, which is not emulated.
        break;
    case 14: {
        // Random ($73A-$74E, $2E3-$2EC): the seed's high byte times
        // R4 << 8 | $8D, the product's high byte the seed's new high byte
        // and its low byte added in, then $8B; sent low byte first.
        unsigned product = ((seed_ >> 8) * (static_cast<unsigned>(r4_) << 8 | 0x8D)) & 0xFFFF;
        seed_ = static_cast<uint16_t>((product & 0xFF00) | (seed_ & 0x00FF));
        seed_ = static_cast<uint16_t>(seed_ + (product & 0xFF) + 0x8B);
        reply(seed_ & 0xFF, 8);
        reply(seed_ >> 8, 8);
        break;
    }
    case 15:
        // Test: the byte sent back ($2F0-$2F2).
        reply(in_, 8);
        break;
    default:
        break;
    }
}

void IPC::reply(unsigned value, int bits) {
    for (int i = bits - 1; i >= 0; i--) out_.push_back(static_cast<uint8_t>((value >> i) & 1));
}

} // namespace ql
