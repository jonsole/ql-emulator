#pragma once
// A plain memory for tests that run a few words of program on the CPU.

#include "m68008.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace qltest {

/// 16 MB that reads as zero until written, counting the CPU's accesses.
class Memory : public ql::Bus {
public:
    uint8_t read(const ql::BusCycle& cycle) override {
        if (cycle.function_code == ql::FunctionCode::InterruptAcknowledge) acknowledges.push_back(cycle);
        reads++;
        auto found = bytes_.find(cycle.address);
        return found == bytes_.end() ? 0 : found->second;
    }
    void write(const ql::BusCycle& cycle, uint8_t data) override {
        writes++;
        bytes_[cycle.address] = data;
    }

    void set_word(uint32_t address, uint16_t value) {
        bytes_[address] = static_cast<uint8_t>(value >> 8);
        bytes_[address + 1] = static_cast<uint8_t>(value);
    }
    uint16_t word(uint32_t address) const {
        auto byte = [this](uint32_t a) -> uint16_t {
            auto found = bytes_.find(a);
            return found == bytes_.end() ? 0 : found->second;
        };
        return static_cast<uint16_t>(byte(address) << 8 | byte(address + 1));
    }

    /// Byte cycles so far: on the 68008, two to a word.
    int reads = 0;
    int writes = 0;
    /// The interrupt acknowledge cycles, which are reads too.
    std::vector<ql::BusCycle> acknowledges;

private:
    std::unordered_map<uint32_t, uint8_t> bytes_;
};

} // namespace qltest
