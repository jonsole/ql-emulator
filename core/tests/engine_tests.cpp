// The Engine: the one live QL, run on its own thread, with every command to
// it serialised through a queue -- zx-spectrum-emulator's design, cut down to
// what this stage needs.
//
// The tests run a hand-built ROM that counts in D0 and stores the count in
// RAM; what each command must leave behind is worked out from that program.

#include "engine.h"
#include "test_main.h"
#include "test_rom.h"

#include <chrono>
#include <cstdint>
#include <thread>

using qltest::COUNTER;
using qltest::counting_rom;
using qltest::rom_running;

namespace {

uint32_t long_at(ql::Engine& engine, uint32_t address) {
    std::vector<uint8_t> bytes = engine.read_memory(address, 4);
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}

} // namespace

// A run keeps going on the Engine's thread until paused, and the queue is
// still served while it does: the counter is read as it climbs. Paused, the
// machine stays where it stopped, with D0 the stored count or the one just
// added and not yet stored.
TEST(run_counts_until_paused) {
    ql::Engine engine(counting_rom(), 128);
    engine.set_uncapped(true);
    std::thread runner([&engine] { engine.run(); });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (long_at(engine, COUNTER) < 1000 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    engine.pause();
    runner.join();

    CHECK(!engine.running());
    uint32_t stored = long_at(engine, COUNTER);
    CHECK(stored >= 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(long_at(engine, COUNTER), stored);
    uint32_t d0 = engine.state().registers.d[0];
    CHECK(d0 == stored || d0 == stored + 1);
}

// Step runs whole instructions. From reset, one step is the MOVEQ: D0 is 0 and
// the ADDQ at $102 is next. Three more -- ADDQ, MOVE, BRA -- store 1 and come
// back round to the ADDQ: its opcode in IR, PC (the next prefetch) at $106.
// Reset starts again from the vector: the MOVEQ at $100 next, PC at $104.
TEST(step_runs_instructions_and_reset_starts_again) {
    ql::Engine engine(counting_rom(), 128);
    ql::MachineState s = engine.step(1);
    CHECK_EQ(s.registers.d[0], 0u);
    CHECK_EQ(s.registers.ir, uint16_t{0x5280});
    CHECK_EQ(s.registers.pc, 0x000106u);

    s = engine.step(3);
    CHECK_EQ(s.registers.d[0], 1u);
    CHECK_EQ(long_at(engine, COUNTER), 1u);
    CHECK_EQ(s.registers.ir, uint16_t{0x5280});
    CHECK_EQ(s.registers.pc, 0x000106u);

    s = engine.reset();
    CHECK_EQ(s.registers.ir, uint16_t{0x7000});
    CHECK_EQ(s.registers.pc, 0x000104u);
    CHECK_EQ(s.registers.ssp, qltest::ROM_INITIAL_SSP);
}

// The screen is published as the machine runs and read without waiting for
// the queue: a ROM that writes $FFFF to the first screen word -- 8 white
// pixels in mode 4 -- and then loops is seen white while still running.
TEST(the_screen_is_seen_while_running) {
    ql::Engine engine(rom_running({
                          0x33FC, 0xFFFF, 0x0002, 0x0000,   // MOVE.W #$FFFF,($00020000).L
                          0x60FE,                           // BRA.S *
                      }),
                      128);
    engine.set_uncapped(true);
    CHECK_EQ(engine.screen()[0], 0x000000u);
    std::thread runner([&engine] { engine.run(); });

    bool seen_while_running = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.screen()[0] == 0xFFFFFFu) {
            seen_while_running = engine.running();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    engine.pause();
    runner.join();
    CHECK(seen_while_running);
    CHECK_EQ(engine.screen()[7], 0xFFFFFFu);
    CHECK_EQ(engine.screen()[8], 0x000000u);
}

RUN_TESTS()
