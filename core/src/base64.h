#pragma once
// Copied from zx-spectrum-emulator (cpp-core/src/base64.h) and left to drift.
// Standard base64, as DAP's readMemory/writeMemory use it.

#include <cstdint>
#include <string>
#include <vector>

namespace ql {

std::string base64_encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base64_decode(const std::string& text);

} // namespace ql
