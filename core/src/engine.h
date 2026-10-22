#pragma once
// The Engine: owns the one live QL and serialises access to it.
//
// zx-spectrum-emulator's design, cut down to what Stage 2 needs. The machine
// runs on its own thread, and everything that touches it goes through a
// queue, so MCP and (later) DAP clients can share one machine without locking
// it against each other.
//
// A `run` is itself a queued job that does not return until it is paused, so
// it cannot hold the thread and ignore everything else: at each yield, every
// RUN_YIELD_EVERY instructions, it serves the queue's other jobs, so reading
// memory reaches a machine that is running, and the run carries on. Anything
// that drives emulation itself -- run, step, reset -- is not served that way,
// since it would nest one emulation loop inside another; those wait for the
// run to end. Pause, and reading the screen, bypass the queue altogether: an
// atomic and a mutexed snapshot.
//
// Unlike zx's, it holds the QL directly, with no machine interface: there is
// one machine here.

#include "ql.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ql {

/// What the machine looks like between instructions.
struct MachineState {
    Registers registers;
    uint64_t clock;   ///< CPU clocks since power-on
    bool stopped;     ///< by STOP, waiting for an interrupt
};

class Engine {
public:
    /// A QL with this system ROM and RAM size (see QL), reset and ready.
    Engine(std::vector<uint8_t> rom, unsigned ram_kb);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- queued: these wait for the Engine's thread ------------------------
    /// Runs until paused, then returns where it stopped.
    MachineState run();
    /// Runs this many whole instructions.
    MachineState step(uint32_t instructions);
    /// The reset button: the CPU's reset sequence, RAM left as it was.
    MachineState reset();
    MachineState state();
    /// Bytes from `address` on, as the CPU would read them.
    std::vector<uint8_t> read_memory(uint32_t address, uint32_t length);

    // ---- queue-bypassing: safe while a run is in flight --------------------
    void pause() { pause_requested_.store(true); }
    bool running() const { return running_.load(); }
    /// Off: realtime, 7.5 MHz against the wall clock. On: as fast as it goes.
    void set_uncapped(bool uncapped) { uncapped_.store(uncapped); }
    /// The screen as last published: after every command, and every 20 ms or
    /// so while running.
    Screen screen() const;

private:
    struct Job {
        std::function<void(QL&)> fn;
        /// Whether a run may serve it at a yield. False for jobs that drive
        /// emulation themselves.
        bool during_run;
    };

    void actor_loop();
    /// Queues `fn` and waits for it.
    void submit(std::function<void(QL&)> fn, bool during_run = true);
    /// Serves the queued jobs a run may: those marked during_run.
    void service_queue();
    void pace_reset();
    /// Holds a run back to realtime, unless uncapped.
    void pace_wait();
    MachineState snapshot() const;
    /// Renders the screen into the published copy.
    void publish_screen();

    QL machine_;

    mutable std::mutex screen_mutex_;
    Screen published_screen_;
    std::chrono::steady_clock::time_point last_published_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Job> queue_;
    bool shutting_down_ = false;

    std::atomic<bool> pause_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> uncapped_{false};
    std::chrono::steady_clock::time_point pace_origin_;
    uint64_t pace_origin_clock_ = 0;

    std::thread thread_;
};

} // namespace ql
