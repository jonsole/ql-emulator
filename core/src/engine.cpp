#include "engine.h"

#include <future>
#include <utility>

namespace ql {

namespace {

/// Instructions between a run's yields: often enough that queued reads and a
/// pause are answered within well under a millisecond, rarely enough that the
/// run loop is not all bookkeeping. zx-spectrum-emulator's figure.
constexpr uint64_t RUN_YIELD_EVERY = 2000;

/// How often a run publishes the screen: 50 times a second, the QL's own rate.
constexpr std::chrono::milliseconds SCREEN_PUBLISH_INTERVAL{20};

/// The QL's 68008 at 7.5 MHz, for pacing a run against the wall clock.
constexpr double CLOCKS_PER_SECOND = 7'500'000.0;

} // namespace

Engine::Engine(std::vector<uint8_t> rom, unsigned ram_kb) : machine_(std::move(rom), ram_kb) {
    machine_.reset();
    publish_screen();
    thread_ = std::thread([this] { actor_loop(); });
}

void Engine::publish_screen() {
    Screen screen = machine_.screen();
    std::lock_guard<std::mutex> lock(screen_mutex_);
    published_screen_ = std::move(screen);
    last_published_ = std::chrono::steady_clock::now();
}

Screen Engine::screen() const {
    std::lock_guard<std::mutex> lock(screen_mutex_);
    return published_screen_;
}

Engine::~Engine() {
    pause();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutting_down_ = true;
    }
    queue_cv_.notify_one();
    thread_.join();
}

void Engine::actor_loop() {
    for (;;) {
        std::function<void(QL&)> job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return shutting_down_ || !queue_.empty(); });
            if (shutting_down_ && queue_.empty()) return;
            job = std::move(queue_.front().fn);
            queue_.pop_front();
        }
        job(machine_);
        // After every command, not just a run: a debugger single-stepping
        // still wants the screen to follow.
        publish_screen();
    }
}

void Engine::submit(std::function<void(QL&)> fn, bool during_run) {
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(Job{[fn = std::move(fn), &done](QL& m) {
                                 fn(m);
                                 done.set_value();
                             },
                             during_run});
    }
    queue_cv_.notify_one();
    finished.wait();
}

void Engine::service_queue() {
    // Only the jobs a run may serve, in order; the rest wait for it to end.
    for (;;) {
        std::function<void(QL&)> job;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            auto it = queue_.begin();
            while (it != queue_.end() && !it->during_run) ++it;
            if (it == queue_.end()) return;
            job = std::move(it->fn);
            queue_.erase(it);
        }
        job(machine_);
    }
}

void Engine::pace_reset() {
    pace_origin_ = std::chrono::steady_clock::now();
    pace_origin_clock_ = machine_.cpu().clock();
}

void Engine::pace_wait() {
    if (uncapped_.load()) {
        pace_reset();
        return;
    }
    double emulated = double(machine_.cpu().clock() - pace_origin_clock_) / CLOCKS_PER_SECOND;
    auto target = pace_origin_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(emulated));
    if (target > std::chrono::steady_clock::now()) std::this_thread::sleep_until(target);
}

MachineState Engine::snapshot() const {
    const M68008& cpu = machine_.cpu();
    return MachineState{cpu.registers(), cpu.clock(), cpu.stopped()};
}

MachineState Engine::run() {
    pause_requested_.store(false);
    running_.store(true);
    MachineState state{};
    submit(
        [this, &state](QL& m) {
            pace_reset();
            uint64_t count = 0;
            while (!pause_requested_.load()) {
                m.cpu().step();
                if (++count % RUN_YIELD_EVERY == 0) {
                    // A frame every 20 ms is all a viewer can use; rendering
                    // at every yield would cost the run more than it gives.
                    if (std::chrono::steady_clock::now() - last_published_ >= SCREEN_PUBLISH_INTERVAL) {
                        publish_screen();
                    }
                    service_queue();
                    pace_wait();
                }
            }
            running_.store(false);
            state = snapshot();
        },
        /*during_run=*/false);
    running_.store(false);
    return state;
}

MachineState Engine::step(uint32_t instructions) {
    MachineState state{};
    submit(
        [this, instructions, &state](QL& m) {
            for (uint32_t i = 0; i < instructions; i++) m.cpu().step();
            state = snapshot();
        },
        /*during_run=*/false);
    return state;
}

MachineState Engine::reset() {
    MachineState state{};
    submit(
        [this, &state](QL& m) {
            m.reset();
            state = snapshot();
        },
        /*during_run=*/false);
    return state;
}

MachineState Engine::state() {
    MachineState state{};
    submit([this, &state](QL&) { state = snapshot(); });
    return state;
}

std::vector<uint8_t> Engine::read_memory(uint32_t address, uint32_t length) {
    std::vector<uint8_t> bytes;
    submit([&](QL& m) {
        bytes.reserve(length);
        for (uint32_t i = 0; i < length; i++) bytes.push_back(m.peek(address + i));
    });
    return bytes;
}

} // namespace ql
