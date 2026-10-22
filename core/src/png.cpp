#include "png.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

namespace ql {

namespace {

void collect(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

std::vector<uint8_t> encode_png(const Screen& screen) {
    // stb wants packed RGB, three bytes a pixel; the screen is 0xRRGGBB.
    std::vector<uint8_t> rgb;
    rgb.reserve(screen.size() * 3);
    for (uint32_t pixel : screen) {
        rgb.push_back(static_cast<uint8_t>(pixel >> 16));
        rgb.push_back(static_cast<uint8_t>(pixel >> 8));
        rgb.push_back(static_cast<uint8_t>(pixel));
    }
    std::vector<uint8_t> png;
    stbi_write_png_to_func(collect, &png, int(SCREEN_WIDTH), int(SCREEN_HEIGHT), 3, rgb.data(),
                           int(SCREEN_WIDTH) * 3);
    return png;
}

} // namespace ql
