// The CPU gate: SingleStepTests/m68000, run against the 68008 core.
//
//   m68000_suite            a fixed sample of every file below (the fast suite)
//   m68000_suite --full     every test in every file (labelled slow)
//   m68000_suite NOP ...    just the files named, for working on one
//
// Each test gives a 68000's state before and after one instruction, every bus
// cycle in between, and the clocks it took. The core runs the instruction on a
// bus that records what it does, and everything is compared: the registers,
// the prefetch queue, memory, and the bus cycles in order with the clock each
// began at. The 68008's cycles and clocks are not in the file; they are
// derived from the 68000's by m68008_timing.h, never captured from the core.
//
// Tests that take an address error are set aside and counted apart until the
// group 0 exception exists (ticket #10), so the instruction tickets can pass
// everything else first.
//
// Exit code 77 -- reported by CTest as skipped -- means the suite has not been
// fetched: run scripts\fetch-m68000-tests.ps1.

#include "m68000_file.h"
#include "m68008.h"
#include "m68008_timing.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ql::m68000_suite;

namespace {

/// The files the core is expected to pass, named as the suite names them.
/// Each instruction ticket adds its own.
const char* const FILES[] = {
    "NOP",
    "MOVE.q",
};

/// The fast suite runs every SAMPLE_STRIDE-th test of each file: a fixed
/// sample, so a failure in it is the same failure on every run.
constexpr size_t SAMPLE_STRIDE = 10;

/// How many failures to describe per file before only counting them.
constexpr int FAILURES_SHOWN = 3;

constexpr int EXIT_SKIPPED = 77;

/// Memory as the test lists it, and a record of every byte cycle.
class RecordingBus : public ql::Bus {
public:
    explicit RecordingBus(const std::vector<RamWord>& ram) {
        for (const RamWord& w : ram) {
            memory_[w.address] = static_cast<uint8_t>(w.value >> 8);
            memory_[w.address + 1] = static_cast<uint8_t>(w.value);
        }
    }

    uint8_t read(const ql::BusCycle& cycle) override {
        // A byte the test does not list reads as zero. The suite lists every
        // byte the instruction touches, so the comparison of cycles catches a
        // read from anywhere else.
        auto found = memory_.find(cycle.address);
        uint8_t data = found == memory_.end() ? 0 : found->second;
        record(Kind::Read, cycle, data);
        return data;
    }

    void write(const ql::BusCycle& cycle, uint8_t data) override {
        memory_[cycle.address] = data;
        record(Kind::Write, cycle, data);
    }

    uint16_t word_at(uint32_t address) const {
        auto byte = [this](uint32_t a) -> uint16_t {
            auto found = memory_.find(a);
            return found == memory_.end() ? 0 : found->second;
        };
        return static_cast<uint16_t>(byte(address) << 8 | byte(address + 1));
    }

    const std::vector<ByteCycle>& cycles() const { return cycles_; }

private:
    void record(Kind kind, const ql::BusCycle& cycle, uint8_t data) {
        cycles_.push_back({kind, cycle.clock, static_cast<uint32_t>(cycle.function_code), cycle.address, data});
    }

    std::unordered_map<uint32_t, uint8_t> memory_;
    std::vector<ByteCycle> cycles_;
};

ql::Registers registers_from(const State& s) {
    ql::Registers r{};
    for (int i = 0; i < 8; i++) r.d[i] = s.d[i];
    for (int i = 0; i < 7; i++) r.a[i] = s.a[i];
    r.usp = s.usp;
    r.ssp = s.ssp;
    r.sr = static_cast<uint16_t>(s.sr);
    r.pc = s.pc;
    r.ir = static_cast<uint16_t>(s.prefetch[0]);
    r.irc = static_cast<uint16_t>(s.prefetch[1]);
    return r;
}

std::string hex(uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%06llx", static_cast<unsigned long long>(v));
    return text;
}

const char* kind_name(Kind k) {
    switch (k) {
    case Kind::Idle: return "idle";
    case Kind::Write: return "write";
    case Kind::Read: return "read";
    case Kind::Tas: return "tas";
    case Kind::ReadAddressError: return "read-address-error";
    case Kind::WriteAddressError: return "write-address-error";
    }
    return "?";
}

std::string describe(const ByteCycle& c) {
    return std::string(kind_name(c.kind)) + " " + hex(c.address) + " = " + hex(c.data).substr(4)
           + " fc " + std::to_string(c.function_code) + " at clock " + std::to_string(c.clock);
}

/// Runs one test; returns an empty string if it passed, or the first
/// difference found.
std::string run_case(const Case& c) {
    RecordingBus bus(c.initial.ram);
    ql::M68008 cpu(bus);
    cpu.set_registers(registers_from(c.initial));
    try {
        cpu.step();
    } catch (const std::exception& e) {
        return std::string("threw: ") + e.what();
    }

    ql::Registers got = cpu.registers();
    ql::Registers want = registers_from(c.final);
    auto reg = [](const char* name, uint32_t g, uint32_t w) -> std::string {
        if (g == w) return "";
        return std::string(name) + " is " + hex(g) + ", expected " + hex(w);
    };
    const char* const d_names[8] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
    const char* const a_names[7] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6"};
    std::string diff;
    for (int i = 0; i < 8 && diff.empty(); i++) diff = reg(d_names[i], got.d[i], want.d[i]);
    for (int i = 0; i < 7 && diff.empty(); i++) diff = reg(a_names[i], got.a[i], want.a[i]);
    if (diff.empty()) diff = reg("usp", got.usp, want.usp);
    if (diff.empty()) diff = reg("ssp", got.ssp, want.ssp);
    if (diff.empty()) diff = reg("sr", got.sr, want.sr);
    if (diff.empty()) diff = reg("pc", got.pc, want.pc);
    if (diff.empty()) diff = reg("ir", got.ir, want.ir);
    if (diff.empty()) diff = reg("irc", got.irc, want.irc);
    if (!diff.empty()) return diff;

    for (const RamWord& w : c.final.ram) {
        uint16_t g = bus.word_at(w.address);
        if (g != w.value) return "memory at " + hex(w.address) + " is " + hex(g) + ", expected " + hex(w.value);
    }

    Expected68008 expected = derive_68008(c.transactions);
    const std::vector<ByteCycle>& cycles = bus.cycles();
    for (size_t i = 0; i < cycles.size() || i < expected.cycles.size(); i++) {
        if (i >= cycles.size()) return "missing cycle " + std::to_string(i) + ": " + describe(expected.cycles[i]);
        if (i >= expected.cycles.size()) return "extra cycle " + std::to_string(i) + ": " + describe(cycles[i]);
        const ByteCycle& g = cycles[i];
        const ByteCycle& w = expected.cycles[i];
        if (g.kind != w.kind || g.clock != w.clock || g.function_code != w.function_code || g.address != w.address
            || g.data != w.data) {
            return "cycle " + std::to_string(i) + " is " + describe(g) + ", expected " + describe(w);
        }
    }
    if (cpu.clock() != expected.length) {
        return "took " + std::to_string(cpu.clock()) + " clocks, expected " + std::to_string(expected.length);
    }
    return "";
}

bool takes_address_error(const Case& c) {
    for (const Transaction& t : c.transactions) {
        if (t.kind == Kind::ReadAddressError || t.kind == Kind::WriteAddressError) return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    bool full = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--full") {
            full = true;
        } else {
            files.push_back(arg);
        }
    }
    if (files.empty()) files.assign(std::begin(FILES), std::end(FILES));

    std::filesystem::path suite =
        (std::filesystem::path(QL_PROJECT_ROOT) / ".test-suites" / "m68000" / "v1").lexically_normal().make_preferred();
    if (!std::filesystem::is_directory(suite)) {
        std::printf("skipped: SingleStepTests/m68000 is not at %s -- run scripts\\fetch-m68000-tests.ps1\n",
                    suite.string().c_str());
        return EXIT_SKIPPED;
    }

    int files_failed = 0;
    for (const std::string& name : files) {
        std::vector<Case> cases;
        try {
            cases = read_test_file((suite / (name + ".json.bin")).string());
        } catch (const std::exception& e) {
            std::printf("%s: %s\n", name.c_str(), e.what());
            files_failed++;
            continue;
        }
        size_t passed = 0, failed = 0, set_aside = 0;
        for (size_t i = 0; i < cases.size(); i += full ? 1 : SAMPLE_STRIDE) {
            const Case& c = cases[i];
            if (takes_address_error(c)) {
                set_aside++;
                continue;
            }
            std::string diff = run_case(c);
            if (diff.empty()) {
                passed++;
            } else {
                if (failed < FAILURES_SHOWN) std::printf("  %s: %s\n", c.name.c_str(), diff.c_str());
                failed++;
            }
        }
        std::printf("%s: %zu passed, %zu failed, %zu set aside as address errors\n", name.c_str(), passed, failed,
                    set_aside);
        if (failed > 0) files_failed++;
    }
    return files_failed == 0 ? 0 : 1;
}
