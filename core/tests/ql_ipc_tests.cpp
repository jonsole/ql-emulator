// The IPC, the QL's 8049, as the 68008 sees it over the ZX8302's link: its
// commands' replies, and the keyboard it scans.
//
// The link is driven as the JS ROM drives it (its routines at $2F7C, $2F96 and
// $2F9A): one bit per write to $18003 -- %11x0 to send the bit x, %1110 to
// listen -- then a wait for bit 6 of $18020, busy, to clear, and the IPC's bit
// read from bit 7. Bits go most significant first; a byte is two nibbles, the
// high one first.
//
// What each command replies is read off the IPC ROM's disassembly (ipc8049.txt,
// JV 970715, from Dilwyn Jones's disassemblies page); the addresses in the
// comments are the 8049's.

#include "ql.h"
#include "test_main.h"
#include "test_rom.h"

#include <cstdint>
#include <vector>

using qltest::rom_running;

namespace {

/// The ZX8302's link register: written to send a bit to the IPC.
constexpr uint32_t LINK_WRITE = 0x018003;
/// The ZX8302's status register: bit 6 busy, bit 7 the bit from the IPC.
constexpr uint32_t LINK_STATUS = 0x018020;

void write_byte(ql::QL& machine, uint32_t address, uint8_t value) {
    machine.write(ql::BusCycle{address, ql::FunctionCode::SupervisorData, machine.cpu().clock()}, value);
}

uint8_t read_byte(ql::QL& machine, uint32_t address) {
    return machine.read(ql::BusCycle{address, ql::FunctionCode::SupervisorData, machine.cpu().clock()});
}

/// Sends the low `bits` bits of `value` to the IPC, most significant first.
void send(ql::QL& machine, unsigned value, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
        uint8_t bit = static_cast<uint8_t>((value >> i) & 1);
        write_byte(machine, LINK_WRITE, static_cast<uint8_t>(0x0C | bit << 1));
        CHECK_EQ(read_byte(machine, LINK_STATUS) & 0x40, 0);
    }
}

/// Receives `bits` bits from the IPC, most significant first.
unsigned receive(ql::QL& machine, int bits) {
    unsigned value = 0;
    for (int i = 0; i < bits; i++) {
        write_byte(machine, LINK_WRITE, 0x0E);
        uint8_t status = read_byte(machine, LINK_STATUS);
        CHECK_EQ(status & 0x40, 0);
        value = value << 1 | (status >> 7);
    }
    return value;
}

/// The IPC's 2,400 or so keyboard scans a second, in 68008 clocks: its idle
/// loop is about 308 machine cycles of 11 MHz / 15, 308 x 7.5 MHz / 733,333.
constexpr uint64_t CLOCKS_PER_SCAN = 3'150;

/// Runs to halfway between the IPC's scans `k` and `k + 1`, scan 0 being at
/// power-on: scan k has been made, and k + 1 has not.
void run_to_scan(ql::QL& machine, uint64_t k) {
    uint64_t until = k * CLOCKS_PER_SCAN + CLOCKS_PER_SCAN / 2;
    while (machine.cpu().clock() < until) machine.step();
}

/// Command 1: the IPC's status byte.
unsigned status(ql::QL& machine) {
    send(machine, 1, 4);
    return receive(machine, 8);
}

/// Command 8, read keyboard, expecting one key: its modifier nibble and
/// keycode as $mkk. The first nibble's bit 3, held, is not checked.
unsigned read_one_key(ql::QL& machine) {
    send(machine, 8, 4);
    CHECK_EQ(receive(machine, 4) & 7, 1u);
    unsigned modifiers = receive(machine, 4);
    return modifiers << 8 | receive(machine, 8);
}

/// A QL whose ROM waits in a loop, the IPC scanning its keyboard meanwhile.
struct IdleMachine : ql::QL {
    IdleMachine() : ql::QL(rom_running({0x60FE}), 128) {   // BRA.S *
        reset();
    }
};

} // namespace

// Command 15, test, takes a byte and sends it back ($2F0-$2F4).
TEST(the_test_command_echoes_its_byte) {
    IdleMachine machine;
    send(machine, 15, 4);
    send(machine, 0xA5, 8);
    CHECK_EQ(receive(machine, 8), 0xA5u);
    send(machine, 15, 4);
    send(machine, 0x3C, 8);
    CHECK_EQ(receive(machine, 8), 0x3Cu);
}

// A key held through a frame -- some 47 of the IPC's scans -- has been taken
// into its keyboard buffer. Command 1, status, says so: bit 0, keys to read,
// and bit 3, a key held, which also sets bit 0 ($228-$236). Command 8 replies
// with a nibble, bit 3 held and bits 2-0 how many keys follow, then each key's
// modifier nibble and keycode ($24D-$26A). The keycode is ((7 - row) << 3) |
// column for KEYROW row and column ($5EC-$5F7): A is row 4, column 4, so $1C.
// Reading empties the buffer ($26C); once A is let go, nothing is held.
TEST(a_key_held_is_reported_by_status_and_read_keyboard) {
    IdleMachine machine;
    machine.press_key(ql::QLKey::A);
    machine.run_frames(1);
    send(machine, 1, 4);
    CHECK_EQ(receive(machine, 8), 0x09u);
    send(machine, 8, 4);
    CHECK_EQ(receive(machine, 4), 0x9u);
    CHECK_EQ(receive(machine, 4), 0x0u);
    CHECK_EQ(receive(machine, 8), 0x1Cu);
    send(machine, 8, 4);
    CHECK_EQ(receive(machine, 4), 0x8u);

    machine.release_key(ql::QLKey::A);
    machine.run_frames(1);
    send(machine, 1, 4);
    CHECK_EQ(receive(machine, 8), 0x00u);
    send(machine, 8, 4);
    CHECK_EQ(receive(machine, 4), 0x0u);
}

// A key is debounced: the first scan to find it alone starts a count, the
// next four count it, the sixth buffers it ($55B-$5A6) and the seventh on
// find it held ($5B8). Pressed after scan 10, A is found by scans 11-15
// without being buffered, buffered by scan 16, and held from scan 17.
TEST(a_key_is_buffered_on_its_sixth_scan_and_held_from_its_seventh) {
    IdleMachine machine;
    run_to_scan(machine, 10);
    machine.press_key(ql::QLKey::A);
    run_to_scan(machine, 15);
    CHECK_EQ(status(machine), 0x00u);
    run_to_scan(machine, 16);
    CHECK_EQ(status(machine), 0x01u);
    run_to_scan(machine, 17);
    CHECK_EQ(status(machine), 0x09u);
}

// SHIFT, CTRL and ALT are not keys of their own: they are row 7's columns 0-2,
// and a key buffered while they are down carries them in its modifier nibble,
// SHIFT 4, CTRL 2 and ALT 1 (the table at $5C0; Minerva's inc/ipcmd). Each is
// held a frame before the key, so that the scan that sees it go down is not
// the one that sees the key.
TEST(shift_ctrl_and_alt_go_in_the_modifier_nibble) {
    IdleMachine machine;
    struct Case {
        ql::QLKey key;
        std::vector<ql::QLKey> modifiers;
        unsigned expected;
    };
    const Case cases[] = {
        {ql::QLKey::A, {ql::QLKey::Shift}, 0x41Cu},
        {ql::QLKey::S, {ql::QLKey::Ctrl}, 0x223u},      // S: row 3, column 3
        {ql::QLKey::D, {ql::QLKey::Alt}, 0x11Eu},       // D: row 4, column 6
        {ql::QLKey::Enter, {ql::QLKey::Shift, ql::QLKey::Ctrl, ql::QLKey::Alt}, 0x730u},   // ENTER: row 1, column 0
        {ql::QLKey::Space, {}, 0x036u},                 // SPACE: row 1, column 6
    };
    for (const Case& c : cases) {
        for (ql::QLKey modifier : c.modifiers) machine.press_key(modifier);
        machine.run_frames(1);
        machine.press_key(c.key);
        machine.run_frames(1);
        CHECK_EQ(read_one_key(machine), c.expected);
        machine.release_key(c.key);
        for (ql::QLKey modifier : c.modifiers) machine.release_key(modifier);
        machine.run_frames(1);
    }
}

// The IPC has no rollover: a scan that finds two keys down, in one row or in
// two, forgets the key it was debouncing and clears held ($5D9, $528/$531 to
// $53F-$54A). A and S together give nothing; let S go, and A is debounced
// afresh and buffered. A held when S joins it is no longer held, and when S is
// let go it is buffered a second time.
TEST(two_keys_down_together_give_neither) {
    IdleMachine machine;
    machine.press_key(ql::QLKey::A);
    machine.press_key(ql::QLKey::S);
    machine.run_frames(1);
    CHECK_EQ(status(machine), 0x00u);
    machine.release_key(ql::QLKey::S);
    machine.run_frames(1);
    CHECK_EQ(read_one_key(machine), 0x01Cu);

    machine.press_key(ql::QLKey::S);
    machine.run_frames(1);
    CHECK_EQ(status(machine), 0x00u);
    machine.release_key(ql::QLKey::S);
    machine.run_frames(1);
    CHECK_EQ(read_one_key(machine), 0x01Cu);
}

// The keyboard buffer, $2C-$39, holds 7 keys. An 8th is lost, and bit 3 of
// the 7th key's modifier nibble says so ($58D-$5B5; Minerva's inc/ipcmd:
// "lost keys (last set only)"). Q W E R T Y U I typed one at a time unread:
// Q is row 6, column 3, so $0B; W row 5 column 1, $11; E $0C; R $14; T $0E;
// Y $16; U $0F; and I is lost.
TEST(an_eighth_key_unread_is_lost_and_flagged) {
    IdleMachine machine;
    const ql::QLKey keys[] = {ql::QLKey::Q, ql::QLKey::W, ql::QLKey::E, ql::QLKey::R,
                              ql::QLKey::T, ql::QLKey::Y, ql::QLKey::U, ql::QLKey::I};
    for (ql::QLKey key : keys) {
        machine.press_key(key);
        machine.run_frames(1);
        machine.release_key(key);
        machine.run_frames(1);
    }
    send(machine, 8, 4);
    CHECK_EQ(receive(machine, 4), 0x7u);
    const unsigned expected[] = {0x00Bu, 0x011u, 0x00Cu, 0x014u, 0x00Eu, 0x016u, 0x80Fu};
    for (unsigned key : expected) {
        unsigned modifiers = receive(machine, 4);
        CHECK_EQ(modifiers << 8 | receive(machine, 8), key);
    }
    send(machine, 8, 4);
    CHECK_EQ(receive(machine, 4), 0x0u);
}

// Command 9, KEYROW, takes a row in a nibble, its top bit ignored, and sends
// back that row of the matrix as it is, bit n for column n ($276-$285): no
// debounce, any number of keys, and SHIFT, CTRL and ALT as row 7's columns
// 0-2. A (row 4, column 4) and D (row 4, column 6) give $50; SHIFT and CTRL
// $03 in row 7; row 12 is row 4.
TEST(keyrow_reads_a_row_of_the_matrix_as_it_is) {
    IdleMachine machine;
    machine.press_key(ql::QLKey::A);
    machine.press_key(ql::QLKey::D);
    machine.press_key(ql::QLKey::Shift);
    machine.press_key(ql::QLKey::Ctrl);
    const unsigned rows[][2] = {{4, 0x50u}, {7, 0x03u}, {12, 0x50u}, {0, 0x00u}};
    for (const auto& row : rows) {
        send(machine, 9, 4);
        send(machine, row[0], 4);
        CHECK_EQ(receive(machine, 8), row[1]);
    }
}

/// Command 15 with `value`, which the IPC can only echo if the link is still
/// in step with it.
void check_in_step(ql::QL& machine, uint8_t value) {
    send(machine, 15, 4);
    send(machine, value, 8);
    CHECK_EQ(receive(machine, 8), unsigned{value});
}

// The commands that do not concern the keyboard take their parameters and
// send their replies, so the link stays in step, and change nothing a
// program could see (docs/plan.md: sound waits for Stage 6, serial input
// for later):
// - 10, sound, takes 64 bits ($300); 11, kill sound, none ($31F). Status
//   bit 1, sound on, stays 0.
// - 2-5 open and close the serial channels, with neither parameters nor reply
//   ($117-$135).
// - 6 and 7 read a serial channel: a byte, the count of bytes to follow in its
//   low 6 bits, and the bytes -- with nothing received, $00 ($289-$2A8).
// - 12 takes a nibble for P2.3 ($2C3), 13 a nibble for the baud rate ($2CF).
TEST(sound_serial_and_the_rest_keep_the_link_in_step) {
    IdleMachine machine;
    send(machine, 10, 4);
    const uint8_t sound[] = {0x20, 0x10, 0x00, 0x01, 0x32, 0x00, 0x00, 0x00};
    for (uint8_t byte : sound) send(machine, byte, 8);
    check_in_step(machine, 0x5A);
    CHECK_EQ(status(machine), 0x00u);
    send(machine, 11, 4);
    check_in_step(machine, 0x69);
    for (unsigned command = 2; command <= 5; command++) {
        send(machine, command, 4);
        check_in_step(machine, static_cast<uint8_t>(command));
    }
    for (unsigned command = 6; command <= 7; command++) {
        send(machine, command, 4);
        CHECK_EQ(receive(machine, 8), 0x00u);
        check_in_step(machine, static_cast<uint8_t>(command));
    }
    send(machine, 12, 4);
    send(machine, 1, 4);
    check_in_step(machine, 0x0C);
    send(machine, 13, 4);
    send(machine, 1, 4);
    check_in_step(machine, 0x0D);
}

/// Command 14: the random number's two bytes, low first, as $llhh.
unsigned random(ql::QL& machine) {
    send(machine, 14, 4);
    return receive(machine, 16);
}

// Command 14, random, steps a 16-bit seed at $25 (low) and $26 (high) and
// sends it, low byte first ($2E3-$2EC). The step ($73A-$74E): $26 times
// (R4 << 8 | $8D), an 8 x 16-bit multiply ($71F), puts the product's high
// byte in $26 and adds its low byte to the seed, then $8B is added. R4 is
// what the last keyboard scan left in it: the modifier code, with no key down
// ($50F). And each key buffered adds the scan count, $27, to the seed ($5A7).
// Worked by hand from a seed of 0:
// - no keys: $26 = 0, so the product is 0: $008B, then $0116, then
//   $26 = 1: 1 x $008D = $008D, so $26 = $00 and $0116 + $8D + $8B = $012E;
// - SHIFT held, R4 = 4: the same until 1 x $048D = $048D, so $26 = $04 and
//   $0416 + $8D + $8B = $052E;
// - A pressed after scan 10, buffered by scan 16, when the count is 17 since
//   scan 0 counts 1: the seed is $0011, then $0011 + $8B = $009C.
TEST(random_steps_its_seed_as_the_ipc_does) {
    {
        IdleMachine machine;
        CHECK_EQ(random(machine), 0x8B00u);
        CHECK_EQ(random(machine), 0x1601u);
        CHECK_EQ(random(machine), 0x2E01u);
    }
    {
        IdleMachine machine;
        machine.press_key(ql::QLKey::Shift);
        machine.run_frames(1);
        CHECK_EQ(random(machine), 0x8B00u);
        CHECK_EQ(random(machine), 0x1601u);
        CHECK_EQ(random(machine), 0x2E05u);
    }
    {
        IdleMachine machine;
        run_to_scan(machine, 10);
        machine.press_key(ql::QLKey::A);
        run_to_scan(machine, 20);
        machine.release_key(ql::QLKey::A);
        run_to_scan(machine, 30);
        CHECK_EQ(random(machine), 0x9C00u);
    }
}

// Command 0 starts the IPC again from IPCinit, which clears its RAM ($21C,
// $009-$02D): a key typed and not read is gone, and the random number starts
// again from its first value, $008B. The QL's reset does the same, as MAME
// resets every device with the machine.
TEST(command_0_and_the_reset_start_the_ipc_again) {
    IdleMachine machine;
    machine.press_key(ql::QLKey::A);
    machine.run_frames(1);
    machine.release_key(ql::QLKey::A);
    machine.run_frames(1);
    random(machine);
    send(machine, 0, 4);
    CHECK_EQ(status(machine), 0x00u);
    CHECK_EQ(random(machine), 0x8B00u);

    machine.press_key(ql::QLKey::A);
    machine.run_frames(1);
    machine.release_key(ql::QLKey::A);
    machine.run_frames(1);
    machine.reset();
    CHECK_EQ(status(machine), 0x00u);
    CHECK_EQ(random(machine), 0x8B00u);
}

// A change of SHIFT, CTRL or ALT sets status bit 2 and clears held, and until
// a scan finds no other key down, any key is ignored ($515-$51C, $54E-$553):
// A held when SHIFT goes down is not held any more, and is not buffered again
// as a shifted A. Once A is let go, the next scan clears bit 2 ($539-$53E).
TEST(a_change_of_modifiers_shuts_out_a_key_already_down) {
    IdleMachine machine;
    machine.press_key(ql::QLKey::A);
    machine.run_frames(1);
    CHECK_EQ(read_one_key(machine), 0x01Cu);
    machine.press_key(ql::QLKey::Shift);
    machine.run_frames(1);
    CHECK_EQ(status(machine), 0x04u);
    send(machine, 8, 4);
    CHECK_EQ(receive(machine, 4), 0x0u);
    machine.release_key(ql::QLKey::A);
    machine.run_frames(1);
    CHECK_EQ(status(machine), 0x00u);
}

RUN_TESTS()
