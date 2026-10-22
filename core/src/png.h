#pragma once
// The screen as a PNG, for get_screen and (later) the screen stream.
// stb_image_write does the encoding (core/third_party, public domain or MIT;
// see THIRD_PARTY_NOTICES.md), as in zx-spectrum-emulator.

#include "zx8301.h"

#include <cstdint>
#include <vector>

namespace ql {

/// A 512x256 RGB PNG of the screen.
std::vector<uint8_t> encode_png(const Screen& screen);

} // namespace ql
